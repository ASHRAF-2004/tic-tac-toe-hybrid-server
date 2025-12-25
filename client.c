#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#define PIPE_PREFIX "/tmp/player_pipe_"

int main(int argc,char*argv[]){
    if(argc!=2){printf("Usage: %s <player_id>\n",argv[0]); return 1;}
    int player_id=atoi(argv[1]);
    char pipe_name[64];
    sprintf(pipe_name,"%s%d",PIPE_PREFIX,player_id);

    int fd=open(pipe_name,O_RDWR);
    if(fd<0){ perror("pipe"); return 1;}

    char buffer[256];
    while(1){
        int n=read(fd,buffer,sizeof(buffer));
        if(n>0){ buffer[n]='\0'; printf("%s\n",buffer);}
        sleep(1);
    }
    close(fd);
    return 0;
}
