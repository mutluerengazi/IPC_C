#ifndef _MF_H_
#define _MF_H_

//You should not change this file. It is the interface of the MF library.

#define CONFIG_FILENAME "mf.config"
// name of the config file.

// min and max msg length
#define MIN_DATALEN 1 // byte
#define MAX_DATALEN 4096 // bytes
// min and max message size (data length)

// min and max queue size
#define MIN_MQSIZE  16 // KB 
#define MAX_MQSIZE  128 // KB
// MQSIZE should be a multiple of 4KB
// 1 KB is 2^12 bytes = 1024 bytes

// min and max shared memory region size
#define MIN_SHMEMSIZE  512  // in KB
#define MAX_SHMEMSIZE  8192 // in KB
// shared memory size must be a power of 2

#define MAXFILENAME 128 
// max file or shared memory name

#define MAX_MQNAMESIZE 128
// max message queue name size


int mf_init();
int mf_destroy();
int mf_connect();
int mf_disconnect();
int mf_create(char *mqname, int mqsize);
int mf_remove(char *mqname);
int mf_open(char *mqname);
int mf_close(int qid);
int mf_send (int qid, void *bufptr, int datalen);
int mf_recv (int qid, void *bufptr, int bufsize);
int mf_print();

#endif


