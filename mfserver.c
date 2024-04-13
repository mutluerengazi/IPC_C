#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <malloc.h>
#include <assert.h>
#include <string.h>
#include "mf.h"


// write the signal handler function
// it will call mf_destroy()
//

int
main(int argc, char *argv[])
{
    
    printf ("mfserver pid=%d\n", (int) getpid());
    
    // register the signal handler function
    
    
    mf_init(); // will read the config file
    
    while (1)
        sleep(1000);
    
    exit(0);
}

