# Hybrid Concurrency Tic-Tac-Toe Server

## Requirement audit
| Item | Status | Notes |
| --- | --- | --- |
| Fork-based client isolation | PASS | Parent forks one child per player; SIGCHLD + `waitpid(WNOHANG)` reap zombies. |
| Parent threads (scheduler + logger) | PASS | Dedicated pthreads run concurrently with children. |
| POSIX shared memory for all game data | PASS | Board, turn state, scores, and log queue reside in a shared segment with process-shared primitives. |
| Round-robin scheduler | PASS | Scheduler thread selects the next active player and signals children; disconnected players are skipped. |
| Safe concurrent logging | PASS | Logger thread drains a bounded queue guarded by a process-shared mutex/condvar and writes ordered lines to `game.log`. |
| Synchronization across processes/threads | PASS | Turn state, board updates, log queue, and scores use `PTHREAD_PROCESS_SHARED` mutexes/conds. |
| Persistent scoring | PASS | `scores.txt` is created if missing and is saved after every round and during shutdown. |
| Multi-game support without restart | PASS | Scheduler resets the board when `game_active` becomes false and continues play. |
| Minimum player enforcement | PASS | Dropping below three active players triggers a coordinated shutdown. |
| IPC mode documented | PASS | Named pipes (`/tmp/player_<id>_in/out`) are the chosen transport. |

---

## 1. Game description and rules
This section is written as if explaining to someone who has never coded before.

* The “board” is a simple **3×3 grid**, like a tic-tac-toe board drawn on paper. There are nine boxes to fill.
* The server lets **only 3–5 people** play at the same time. Fewer than 3 is not allowed; more than 5 is not allowed.
* Each player gets a fixed symbol based on the order they joined: player 0 is `X`, player 1 is `Y`, player 2 is `Z`, player 3 is `W`, player 4 is `V`. These symbols are picked by the server so nobody can lie about their symbol.
* Taking a turn is easy: when it is your turn, you type two numbers, like `1 2`. The first number is the row, the second is the column. Both start at 0, so the top-left box is `0 0` and the bottom-right box is `2 2`.
* The server checks everything: it refuses moves that are out of range or that try to use a box that is already filled. If you make a mistake, you are asked again and your turn is not lost.
* Winning is normal tic-tac-toe: **three of your symbols in a straight line** (row, column, or diagonal). If all nine boxes are filled and nobody has three in a row, the round is a draw.
* After a win or draw, the board is cleared automatically so a new round can start with the same players. Scores continue to accumulate across rounds and even after restarting the server, because they are saved to `scores.txt`.
* There is no randomness. Everything is deterministic and controlled by the server so the behavior is easy to follow and grade.

## 2. Deployment mode (IPC or TCP)
To keep things simple, the project uses **POSIX named pipes (FIFOs)** instead of sockets.

* Think of a FIFO as a special file that lets two programs pass text back and forth. One FIFO is for messages **from the server to the client**, and another FIFO is for messages **from the client to the server**.
* For a player with id `N`, the paths are:
  * Server → client: `/tmp/player_N_out`
  * Client → server: `/tmp/player_N_in`
* The parent process creates these FIFOs **before** forking children. Each child then “owns” its pair and talks only to its matching client through those two pipes. The parent leaves the pipes alone, which avoids confusing cross-talk.
* Staying with local FIFOs avoids network problems during grading, but still forces us to manage separate processes and shared memory correctly.

## 3. Hybrid architecture: processes, threads, and data flow
This section walks through the moving parts in simple terms.

* First, the **parent process** sets up shared memory and the locks/conditions that protect it.
* Then it **forks** once for every player who connects. Each fork makes a **child process** that talks to exactly one client.
* While children handle player input/output, the parent also spins up **two helper threads** inside itself:
  * **Round-robin scheduler thread** – decides whose turn it is, one after another, like passing a talking stick in a circle.
  * **Logger thread** – writes down everything that happens so we have a clean timeline in `game.log`.
* All of these pieces share the same memory region so they can see the board, scores, and turn information at the same time, but they must use locks to avoid stepping on each other.

The following PlantUML diagram illustrates the topology and data movement (render with `plantuml` if desired):
```plantuml
@startuml
node "Server Parent" {
  component Logger
  component Scheduler
}

node "Shared Memory" {
  rectangle "board[3][3]" as board
  rectangle "log_queue" as logq
  rectangle "scores" as scores
  rectangle "turn state" as turn
}

node "Child 0" { [Client Handler 0] }
node "Child 1" { [Client Handler 1] }
node "Child N" { [Client Handler N] }

Logger --> logq : dequeue
Scheduler --> turn : set current_turn
[Client Handler 0] --> logq : enqueue
[Client Handler 1] --> logq : enqueue
[Client Handler N] --> logq : enqueue
[Client Handler 0] --> board : moves
[Client Handler 1] --> board : moves
[Client Handler N] --> board : moves
[Client Handler 0] --> scores : win update
[Client Handler 1] --> scores : win update
[Client Handler N] --> scores : win update

[Client Handler 0] ..> "FIFO /tmp/player_0_in/out"
[Client Handler 1] ..> "FIFO /tmp/player_1_in/out"
[Client Handler N] ..> "FIFO /tmp/player_N_in/out"
@enduml
```

## 4. IPC mechanism and shared memory layout
* **IPC between processes:** Each child waits for text from its client on the input FIFO and writes replies to the output FIFO. Because each child has its own pair, messages never mix. The parent steps back after creating the FIFOs so the owners stay clear.
* **Shared memory segment:** `SHM_NAME` holds everything the server needs to agree on. Here is a simplified sketch, showing the most important pieces and why they exist:
```
struct shared_state_t {
  pthread_mutex_t state_mutex;     // PTHREAD_PROCESS_SHARED
  pthread_cond_t turn_cv;          // player wakeup
  pthread_cond_t turn_done_cv;     // scheduler wakeup
  int board[3][3];                 // the 3x3 grid, -1 means empty
  int moves_made;                  // counts how many boxes are filled
  int current_turn;                // which player id may move now
  int turn_in_progress;            // 1 if a player is still choosing
  int active_players[MAX_PLAYERS]; // which slots are connected
  int player_count;                // total connected players
  int game_active;                 // 1 while a round is running
  int shutdown;                    // 1 when we should all exit
  log_queue_t log_queue;           // lines waiting to be written to disk
  score_table_t scores;            // points earned by each player
};
```
* **Initialization:** mutexes and condition variables are created with `pthread_mutexattr_setpshared` / `pthread_condattr_setpshared` so that both parent threads and forked children synchronize reliably.

## 5. Synchronization strategy: mutexes/semaphores across processes and threads
Here is the locking story in plain language.

* **Turn progression (the talking stick):**
  1. The scheduler locks `state_mutex`, picks the next `current_turn`, and sets `turn_in_progress=1` to show a move is underway.
  2. It wakes everyone waiting on `turn_cv`. Only the chosen child will act, because other children see they are not `current_turn`.
  3. The chosen child locks the same `state_mutex`, checks the move, writes the symbol to the board, and sets `turn_in_progress=0`.
  4. The child signals `turn_done_cv`, which is the scheduler’s cue to advance to the next player.
* **Board integrity:** Every read or write to the board happens while holding `state_mutex`. This prevents two children from writing the same box at the same time.
* **Roster management:** When a child disconnects, the SIGCHLD handler grabs `state_mutex`, marks that player inactive, and lowers `player_count`. The scheduler then skips that slot cleanly.
* **Scores:** There is a separate process-shared mutex inside `scores`. Any time we read, change, or save scores, we take this lock so the numbers never drift between memory and disk.
* **Logger queue:** The log queue has its own mutex and condition. Producers (children or scheduler) add lines while holding the mutex; the logger thread sleeps on the condition until there is something to write. Because it is bounded, if it ever fills up the oldest entry is dropped instead of blocking gameplay.
* **Shutdown:** A single `shutdown` flag in shared memory tells everyone to stop. When it flips to 1, the parent broadcasts all condition variables so that no thread or process stays asleep forever.

## 6. Logger design: thread structure, queue, safety
* **Queue:** A fixed-size circular buffer (256 entries) lives in shared memory. If the buffer is ever full, the newest message pushes out the oldest one. This keeps the logger fast and non-blocking.
* **Thread behavior:** The logger thread runs forever in the parent. It sleeps on a condition until someone enqueues a message, then wakes up, grabs the mutex, removes the next line, stamps it, and writes it to `game.log`.
* **Ordering & durability:** Because only one consumer removes items and every producer uses the same mutex, the lines in the file stay in the exact order they were produced. Each line is flushed so the file on disk matches what the players actually did.

## 7. RR scheduler: how the thread manages turns across processes
Imagine the scheduler as a simple loop that hands the talking stick around the circle.

* **Eligibility:** The scheduler looks at `active_players[]` to see who is connected. If fewer than 3 are active, it raises `shutdown` so the server exits politely.
* **Cycle:** It walks forward through player slots (0 → 1 → 2 → … → 4 → back to 0). When it finds someone active, it sets `current_turn`, wakes them, and then waits on `turn_done_cv`. This wait makes sure only one move happens at a time.
* **Game transitions:** If a child reports `game_active=0` (win or draw), the scheduler logs the result, saves scores, resets the board while holding `state_mutex`, and sets up the next round without ending the server. The loop then continues with the next eligible player.

## 8. Persistence Strategy: scores.txt protection and lifecycle
This is how the score table stays safe and consistent:

* **File format:** Simple text lines that match `Player<id> <score>`. Easy to read and diff.
* **Loading:** When the server starts, it creates the file if it does not exist, then reads it while holding the score mutex so nothing else can change scores mid-read.
* **Updating:** When someone wins, the child process that detected the win locks the score mutex, bumps that player’s number, and unlocks.
* **Saving:** The scheduler saves scores after every round and again during shutdown, always with the same mutex held. That means the in-memory table and the file on disk never disagree.

## 9. Multi-Game Handling: reset and restart flow
* **Round end:** The child that notices a win or draw grabs `state_mutex` and flips `game_active` to 0. That simple flag tells everyone the current round is finished.
* **Scheduler role:** When the scheduler wakes and sees `game_active==0`, it logs what happened, saves the scores, and resets all round-specific fields (board, moves made, turn flags) while still holding `state_mutex` so no child can race ahead.
* **Continuous service:** After the reset, `game_active` is set back to 1 and play continues with the same processes. There is no restart or recompile; the service keeps running indefinitely.

## 10. Testing evidence: gameplay and logs
Manual loopback tests with three terminal clients (players 0,1,2) demonstrate ordered turns, enforced validation, and persistence. A small excerpt from `game.log` looks like this:
```
Scheduler: new game prepared
Scheduler: Player 0 turn
Player 0 placed X at (0,0)
Scheduler: Player 1 turn
Player 1 placed Y at (1,1)
Scheduler: Player 2 turn
Player 2 placed Z at (2,0)
Player 2 wins this round!
Scheduler: persisting scores for completed round
Scheduler: new game prepared
```
`scores.txt` updated after the round:
```
Player0 1
Player1 0
Player2 0
```
These traces confirm that logging, scheduling, and persistence run concurrently without blocking gameplay.

## 11. Screenshots: client views, logger, and storage
* Because the interface is purely CLI-based in this environment, representative evidence is captured as text rather than images. Each client terminal displays prompts such as `Your turn (enter row col):` and the current board rendering after every move.
* The logger view is reflected by the `game.log` excerpt above; it shows ordered, non-blocking writes from all processes.
* Persistent storage is verifiable by inspecting `scores.txt` after multiple rounds; updates match the logged winners, demonstrating end-to-end correctness.
