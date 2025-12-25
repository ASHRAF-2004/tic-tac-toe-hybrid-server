#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <signal.h>
#include <fcntl.h>
#include <string.h>
#include <semaphore.h>
#include <sys/stat.h>  

#define MAX_PLAYERS 3
#define SHM_NAME "/game_shm"
#define LOG_FILE "game.log"
#define SCORE_FILE "scores.txt"
#define PIPE_PREFIX "/tmp/player_pipe_"
#define BOARD_SIZE 3

typedef struct {
    int current_turn;
    int active_players[MAX_PLAYERS];
    char board[BOARD_SIZE][BOARD_SIZE];
    int moves_made;
    pthread_mutex_t turn_mutex;
} shared_state_t;

shared_state_t *state;

typedef struct {
    char messages[1024][256];
    int head, tail;
    pthread_mutex_t mutex;
} log_queue_t;

log_queue_t log_queue;

typedef struct {
    char player_name[32];
    int score;
} player_score_t;

player_score_t scores[MAX_PLAYERS];
pthread_mutex_t score_mutex;

void log_event(const char *msg){
    pthread_mutex_lock(&log_queue.mutex);
    strcpy(log_queue.messages[log_queue.head], msg);
    log_queue.head=(log_queue.head+1)%1024;
    pthread_mutex_unlock(&log_queue.mutex);
}

void *logger_thread(void *arg){
    FILE *fp=fopen(LOG_FILE,"a");
    if(!fp){ perror("log"); return NULL; }
    while(1){
        pthread_mutex_lock(&log_queue.mutex);
        while(log_queue.head!=log_queue.tail){
            fprintf(fp,"%s\n",log_queue.messages[log_queue.tail]);
            fflush(fp);
            log_queue.tail=(log_queue.tail+1)%1024;
        }
        pthread_mutex_unlock(&log_queue.mutex);
        usleep(100000);
    }
    fclose(fp);
    return NULL;
}

void *scheduler_thread(void *arg){
    while(1){
        pthread_mutex_lock(&state->turn_mutex);
        for(int i=1;i<=MAX_PLAYERS;i++){
            int next=(state->current_turn+i)%MAX_PLAYERS;
            if(state->active_players[next]){
                state->current_turn=next;
                char msg[128];
                sprintf(msg,"Scheduler: Next turn -> Player %d",next);
                log_event(msg);
                break;
            }
        }
        pthread_mutex_unlock(&state->turn_mutex);
        sleep(1);
    }
    return NULL;
}

void sigchld_handler(int sig){
    while(waitpid(-1,NULL,WNOHANG)>0);
}

void load_scores(){
    FILE *fp=fopen(SCORE_FILE,"r");
    if(!fp) return;
    pthread_mutex_lock(&score_mutex);
    for(int i=0;i<MAX_PLAYERS;i++)
        if(fscanf(fp,"%s %d",scores[i].player_name,&scores[i].score)!=2) break;
    pthread_mutex_unlock(&score_mutex);
    fclose(fp);
}

void save_scores(){
    FILE *fp=fopen(SCORE_FILE,"w");
    if(!fp) return;
    pthread_mutex_lock(&score_mutex);
    for(int i=0;i<MAX_PLAYERS;i++)
        if(strlen(scores[i].player_name)>0)
            fprintf(fp,"%s %d\n",scores[i].player_name,scores[i].score);
    pthread_mutex_unlock(&score_mutex);
    fclose(fp);
}

int check_winner(char symbol){
    for(int i=0;i<BOARD_SIZE;i++)
        if(state->board[i][0]==symbol && state->board[i][1]==symbol && state->board[i][2]==symbol) return 1;
    for(int i=0;i<BOARD_SIZE;i++)
        if(state->board[0][i]==symbol && state->board[1][i]==symbol && state->board[2][i]==symbol) return 1;
    if(state->board[0][0]==symbol && state->board[1][1]==symbol && state->board[2][2]==symbol) return 1;
    if(state->board[0][2]==symbol && state->board[1][1]==symbol && state->board[2][0]==symbol) return 1;
    return 0;
}

void handle_client(int player_id){
    char pipe_name[64];
    sprintf(pipe_name,"%s%d",PIPE_PREFIX,player_id);
    mkfifo(pipe_name,0666);
    int fd=open(pipe_name,O_RDWR);
    if(fd<0){ perror("pipe"); exit(1); }

    char buffer[256];
    char symbol='X'+player_id;
    sprintf(scores[player_id].player_name,"Player%d",player_id);

    while(1){
        pthread_mutex_lock(&state->turn_mutex);
        int my_turn=(state->current_turn==player_id);
        pthread_mutex_unlock(&state->turn_mutex);

        if(my_turn){
            int move_done=0;
            for(int r=0;r<BOARD_SIZE && !move_done;r++)
                for(int c=0;c<BOARD_SIZE && !move_done;c++)
                    if(state->board[r][c]==' '){
                        state->board[r][c]=symbol;
                        state->moves_made++;
                        move_done=1;
                        sprintf(buffer,"Player %d placed %c at (%d,%d)",player_id,symbol,r,c);
                        write(fd,buffer,strlen(buffer)+1);
                        log_event(buffer);

                        if(check_winner(symbol)){
                            sprintf(buffer,"Player %d WINS!",player_id);
                            log_event(buffer);
                            pthread_mutex_lock(&score_mutex);
                            scores[player_id].score++;
                            pthread_mutex_unlock(&score_mutex);
                            for(int i=0;i<BOARD_SIZE;i++)
                                for(int j=0;j<BOARD_SIZE;j++) state->board[i][j]=' ';
                            state->moves_made=0;
                        } else if(state->moves_made==BOARD_SIZE*BOARD_SIZE){
                            sprintf(buffer,"Game Draw!");
                            log_event(buffer);
                            for(int i=0;i<BOARD_SIZE;i++)
                                for(int j=0;j<BOARD_SIZE;j++) state->board[i][j]=' ';
                            state->moves_made=0;
                        }
                        break;
                    }
            sleep(1);
        } else sleep(1);
    }
    close(fd);
    unlink(pipe_name);
    exit(0);
}

int main(){
    signal(SIGCHLD,sigchld_handler);
    void handle_sigint(int sig){
    save_scores();
    exit(0);
}

// In main()
signal(SIGINT, handle_sigint);


    int shm_fd=shm_open(SHM_NAME,O_CREAT|O_RDWR,0666);
    ftruncate(shm_fd,sizeof(shared_state_t));
    state=mmap(NULL,sizeof(shared_state_t),PROT_READ|PROT_WRITE,MAP_SHARED,shm_fd,0);

    state->current_turn=0;
    state->moves_made=0;
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr,PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&state->turn_mutex,&attr);

    for(int i=0;i<BOARD_SIZE;i++)
        for(int j=0;j<BOARD_SIZE;j++) state->board[i][j]=' ';
    for(int i=0;i<MAX_PLAYERS;i++) state->active_players[i]=1;

    pthread_mutex_init(&log_queue.mutex,NULL);
    log_queue.head=log_queue.tail=0;
    pthread_mutex_init(&score_mutex,&attr);
    load_scores();

    pthread_t logger_tid,scheduler_tid;
    pthread_create(&logger_tid,NULL,logger_thread,NULL);
    pthread_create(&scheduler_tid,NULL,scheduler_thread,NULL);

    for(int i=0;i<MAX_PLAYERS;i++){
        pid_t pid=fork();
        if(pid==0) handle_client(i);
    }

    while(1) pause();
    return 0;
}
