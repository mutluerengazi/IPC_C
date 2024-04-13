#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <malloc.h>
#include <assert.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <semaphore.h>
#include "mf.h"

// Global variables to store configuration and shared memory information
static char shmem_name[MAXFILENAME];
static int shmem_size;
static int max_msgs_in_queue;
static int max_queues_in_shmem;
static void *shmem_addr;
static sem_t *semaphore_id;

// Shared memory layout structure
typedef struct {
    int num_queues;
    // Add other metadata fields as needed
} shmem_metadata_t;

shmem_metadata_t *shmem_metadata;

int mf_init() {
    FILE *config_file = fopen(CONFIG_FILENAME, "r");
    if (config_file == NULL) {
        perror("Error opening config file");
        return -1;
    }

    // Read configuration parameters from the file
    char line[256];
    while (fgets(line, sizeof(line), config_file)) {
        if (strncmp(line, "SHMEM_NAME", 10) == 0) {
            sscanf(line, "SHMEM_NAME %s", shmem_name);
        } else if (strncmp(line, "SHMEM_SIZE", 10) == 0) {
            sscanf(line, "SHMEM_SIZE %d", &shmem_size);
        } else if (strncmp(line, "MAX_MSGS_IN_QUEUE", 17) == 0) {
            sscanf(line, "MAX_MSGS_IN_QUEUE %d", &max_msgs_in_queue);
        } else if (strncmp(line, "MAX_QUEUES_IN_SHMEM", 19) == 0) {
            sscanf(line, "MAX_QUEUES_IN_SHMEM %d", &max_queues_in_shmem);
        }
    }
    fclose(config_file);
    printf("shmem_name: %s\n", shmem_name);
    printf("SHMEM_SIZE: %d\n", shmem_size);
    printf("MAX_MSGS_IN_QUEUE: %d\n", max_msgs_in_queue);
    printf("MAX_QUEUES_IN_SHMEM: %d\n", max_queues_in_shmem);
    // Create shared memory segment
    int shm_fd = shm_open(shmem_name, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("Error creating shared memory");
        return -1;
    }

    // Set the size of the shared memory segment
    if (ftruncate(shm_fd, shmem_size) == -1) {
        perror("Error setting shared memory size");
        return -1;
    }

    // Map the shared memory into the process's address space
    shmem_addr = mmap(NULL, shmem_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shmem_addr == MAP_FAILED) {
        perror("Error mapping shared memory");
        return -1;
    }

    // Initialize shared memory layout
    shmem_metadata = (shmem_metadata_t *)shmem_addr;
    shmem_metadata->num_queues = 0;
    
    // Initialize the default message queue
    mf_queue_t *queue = (mf_queue_t *)((char *)shmem_addr + sizeof(shmem_metadata_t));
    queue->size = max_msgs_in_queue * MAX_DATALEN;
    strcpy(queue->name, "default_queue");
    queue->head = 0;
    queue->tail = 0;
    queue->count = 0;
    queue->in = 0;
    queue->out = 0;
    // Create synchronization objects (semaphores or mutexes)
    semaphore_id = sem_open("/mf_semaphore", O_CREAT, 0666, 1);
    if (semaphore_id == SEM_FAILED) {
        perror("Error creating semaphore");
        return -1;
    }

    // Initialize other data structures or variables
    // ...

    return 0; // Success
}

int mf_destroy() {
    // Remove synchronization objects (semaphores or mutexes)
    if (sem_unlink("/mf_semaphore") == -1) {
        perror("Error removing semaphore");
        return -1;
    }

    // Unmap the shared memory
    if (munmap(shmem_addr, shmem_size) == -1) {
        perror("Error unmapping shared memory");
        return -1;
    }

    // Remove the shared memory segment
    if (shm_unlink(shmem_name) == -1) {
        perror("Error removing shared memory");
        return -1;
    }

    // Clean up other data structures or resources
    // ...

    return 0; // Success
}
int mf_connect()
{
    return (0);
}
   
int mf_disconnect()
{
    return (0);
}

int mf_create(char *mqname, int mqsize)
{
    return (0);
}

int mf_remove(char *mqname)
{
    return (0);
}


int mf_open(char *mqname)
{
    return (0);
}

int mf_close(int qid)
{
    return(0);
}


int mf_send(int qid, void *bufptr, int datalen) {
    mf_queue_t *queue = (mf_queue_t *)((char *)shmem_addr + sizeof(shmem_metadata_t));

    // Acquire the semaphore or lock
    sem_wait(semaphore_id);

    // Check if there's space in the buffer
    if (queue->count == queue->size) {
        // Buffer is full, handle the error or block
        sem_post(semaphore_id);
        return -1;
    }

    // Copy the message data into the buffer
    memcpy(queue->buffer + queue->in, bufptr, datalen);

    // Update the indices and count
    queue->in = (queue->in + datalen) % queue->size;
    queue->count += datalen;

    // Release the semaphore or lock
    sem_post(semaphore_id);

    return 0;
}
  

int mf_recv(int qid, void *bufptr, int bufsize) {
    mf_queue_t *queue = (mf_queue_t *)((char *)shmem_addr + sizeof(shmem_metadata_t));

    // Acquire the semaphore or lock
    sem_wait(semaphore_id);

    // Check if there are messages in the buffer
    if (queue->count == 0) {
        // Buffer is empty, handle the error or block
        sem_post(semaphore_id);
        return -1;
    }

    // Calculate the message length
    int msg_len = queue->buffer[queue->out];

    // Check if the provided buffer size is sufficient
    if (bufsize < msg_len) {
        // Buffer size is too small, handle the error
        sem_post(semaphore_id);
        return -1;
    }

    // Copy the message data from the buffer
    memcpy(bufptr, queue->buffer + queue->out + sizeof(int), msg_len);

    // Update the indices and count
    queue->out = (queue->out + msg_len + sizeof(int)) % queue->size;
    queue->count -= msg_len + sizeof(int);

    // Release the semaphore or lock
    sem_post(semaphore_id);

    return msg_len;
}

int mf_print()
{
    return (0);
}


