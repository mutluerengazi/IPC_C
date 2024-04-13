#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <malloc.h>
#include <assert.h>
#include <string.h>
#include <signal.h>
#include "mf.h"


// write the signal handler function
// it will call mf_destroy()
//


static void signal_handler(int signo) {
    if (signo == SIGINT || signo == SIGTERM) {
        printf("Received signal %d, cleaning up...\n", signo);
        mf_destroy();
        exit(0);
    }
}

int main(int argc, char *argv[]) {
    printf("mfserver pid=%d\n", (int)getpid());

    // Register signal handler
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Initialize the MF library
    if (mf_init() != 0) {
        fprintf(stderr, "Error initializing MF library\n");
        exit(1);
    }

    // Server main loop
    while (1) {
        sleep(1000);
    }

    return 0;
}