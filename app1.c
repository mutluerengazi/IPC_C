#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <time.h>
#include "mf.h"

#define COUNT 10
char *semname1 = "/semaphore1";
char *semname2 = "/semaphore2";
sem_t *sem1, *sem2;
char *mqname1 = "msgqueue1";

int 
main(int argc, char **argv)
{
    int ret,  i, qid;
    char sendbuffer[MAX_DATALEN];
    int n_sent, n_received;
    char recvbuffer[MAX_DATALEN];
    int sentcount;
    int receivedcount;
    int totalcount;
    
    totalcount = COUNT;
    if (argc == 2)
        totalcount = atoi(argv[1]);

    sem1 = sem_open(semname1, O_CREAT, 0666, 0); // init sem
    sem2 = sem_open(semname2, O_CREAT, 0666, 0); // init sem

    srand(time(0));
    printf ("RAND_MAX is %d\n", RAND_MAX);
    
    ret = fork();
    if (ret > 0) {
        // parent process - P1
        // parent will create a message queue
        
        mf_connect();
        mf_create(mqname1, 16); // Create a message queue with size 16 KB
        qid = mf_open(mqname1);

        sem_post(sem1);

        for (int i = 0; i < totalcount; i++) {
            n_sent = rand() % MAX_DATALEN;
            sprintf(sendbuffer, "Message %d", i); // Example message
            ret = mf_send(qid, sendbuffer, n_sent + 1); // Include null terminator
            if (ret == 0) {
                printf("Parent sent message: %s (length: %d)\n", sendbuffer, n_sent + 1);
            } else {
                printf("Error sending message\n");
            }
        }

        mf_close(qid);
        sem_wait(sem2);

        mf_remove(mqname1); // Remove the message queue
        mf_disconnect();
    }
    else if (ret == 0) {
        // child process - P2
        // child will connect, open mq, use mq
        sem_wait(sem1); // Wait for the parent to create the message queue

        mf_connect();
        qid = mf_open(mqname1);

        for (int i = 0; i < totalcount; i++) {
            memset(recvbuffer, 0, sizeof(recvbuffer));
            n_received = mf_recv(qid, recvbuffer, MAX_DATALEN);
            if (n_received > 0) {
                printf("Child received message: %s (length: %d)\n", recvbuffer, n_received);
            } else {
                printf("Error receiving message\n");
            }
        }

        mf_close(qid);
        mf_disconnect();
        sem_post(sem2);
    }
	return 0;
}