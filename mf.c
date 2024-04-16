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
#include <ctype.h> // for isspace and isdigit functions

// Global variables to store configuration and shared memory information
static char shmem_name[MAXFILENAME];
static int shmem_size;
static int max_msgs_in_queue;
static int max_queues_in_shmem;
static void *shmem_addr;
static sem_t *semaphore_id;
void *global_shmem_addr = NULL;  // Initialize pointer to NULL
int global_shmem_size = 0;       // Initialize size to 0
int shm_fd = -1;                 // Initialize file descriptor to invalid value

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

    char line[256], key[256], value[256];
    
    while (fgets(line, sizeof(line), config_file)) {
        if (line[0] == '#' || isspace(line[0])) continue;
        
        if (sscanf(line, "%s %s", key, value) != 2) {
            fprintf(stderr, "Malformed line in config file: %s", line);
            continue;
        }

        if (strcmp(key, "SHMEM_NAME") == 0) {
            strncpy(shmem_name, value, sizeof(shmem_name) - 1);
            shmem_name[sizeof(shmem_name) - 1] = '\0';
        } else if (strcmp(key, "SHMEM_SIZE") == 0) {
            shmem_size = atoi(value) * 1024; // Convert KB to bytes
        } else if (strcmp(key, "MAX_MSGS_IN_QUEUE") == 0) {
            max_msgs_in_queue = atoi(value);
        } else if (strcmp(key, "MAX_QUEUES_IN_SHMEM") == 0) {
            max_queues_in_shmem = atoi(value);
        }
    }

    fclose(config_file);

    if (shmem_name[0] == '\0' || shmem_size == 0 || max_msgs_in_queue == 0 || max_queues_in_shmem == 0) {
        fprintf(stderr, "Configuration incomplete or invalid.\n");
        return -1;
    }

    int shm_fd = shm_open(shmem_name, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("Error creating shared memory");
        return -1;
    }

    if (ftruncate(shm_fd, shmem_size) == -1) {
        perror("Error setting shared memory size");
        shm_unlink(shmem_name); // Cleanup on failure
        return -1;
    }

    shmem_addr = mmap(NULL, shmem_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shmem_addr == MAP_FAILED) {
        perror("Error mapping shared memory");
        shm_unlink(shmem_name); // Cleanup on failure
        return -1;
    }

    // Initialize shared memory layout
    shmem_metadata = (shmem_metadata_t *)shmem_addr;
    shmem_metadata->num_queues = max_queues_in_shmem;

    // Initialize all message queues within the allocated shared memory
    mf_queue_t *queue = (mf_queue_t *)((char *)shmem_addr + sizeof(shmem_metadata_t));
    for (int i = 0; i < max_queues_in_shmem; i++) {
        queue[i].size = max_msgs_in_queue * MAX_DATALEN;
        sprintf(queue[i].name, "queue_%d", i);
        queue[i].in = 0;
        queue[i].out = 0;
        queue[i].ref_count = 0;
        queue[i].sem_enqueue = sem_open("enqueue_semaphore", O_CREAT, 0644, max_msgs_in_queue);
        queue[i].sem_dequeue = sem_open("dequeue_semaphore", O_CREAT, 0644, 0);
        queue[i].mutex = sem_open("mutex_semaphore", O_CREAT, 0644, 1);
        if (queue[i].sem_enqueue == SEM_FAILED || queue[i].sem_dequeue == SEM_FAILED || queue[i].mutex == SEM_FAILED) {
            perror("Error opening semaphores");
            return -1; // Semaphore initialization failed
        }
    }

    return 0; // Success
}

int mf_destroy() {
    int cleanup_status = 0;

    // Unlink all named semaphores
    if (sem_unlink("enqueue_semaphore") == -1) {
        perror("Error unlinking enqueue semaphore");
        cleanup_status = -1;  // Note the error but continue cleanup
    }
    if (sem_unlink("dequeue_semaphore") == -1) {
        perror("Error unlinking dequeue semaphore");
        cleanup_status = -1;
    }
    if (sem_unlink("mutex_semaphore") == -1) {
        perror("Error unlinking mutex semaphore");
        cleanup_status = -1;
    }

    // Unmap the shared memory
    if (munmap(shmem_addr, shmem_size) == -1) {
        perror("Error unmapping shared memory");
        cleanup_status = -1;
    }

    // Remove the shared memory segment
    if (shm_unlink(shmem_name) == -1) {
        perror("Error removing shared memory");
        cleanup_status = -1;
    }

    return cleanup_status;
}

int mf_connect() {
    FILE *config_file = fopen(CONFIG_FILENAME, "r");
    if (config_file == NULL) {
        perror("Error opening config file");
        return -1;
    }

    char shmem_name[MAXFILENAME];
    int shmem_size = 0;
    char line[256], key[256], value[256];
    
    // Read configuration settings
    while (fgets(line, sizeof(line), config_file)) {
        if (line[0] == '#' || isspace(line[0])) continue; // Skip comments and empty lines
        
        if (sscanf(line, "%s %s", key, value) != 2) {
            fprintf(stderr, "Malformed line in config file: %s", line);
            continue; // Skip malformed lines
        }

        if (strcmp(key, "SHMEM_NAME") == 0) {
            strncpy(shmem_name, value, sizeof(shmem_name) - 1);
            shmem_name[sizeof(shmem_name) - 1] = '\0';
        } else if (strcmp(key, "SHMEM_SIZE") == 0) {
            shmem_size = atoi(value) * 1024; // Convert KB to bytes
        }
    }

    fclose(config_file);

    // Check if required configuration is retrieved
    if (shmem_name[0] == '\0' || shmem_size == 0) {
        fprintf(stderr, "Configuration incomplete or invalid.\n");
        return -1;
    }

    // Attach to the existing shared memory segment
    int shm_fd = shm_open(shmem_name, O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("Error accessing shared memory");
        return -1;
    }

    void *shmem_addr = mmap(NULL, shmem_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shmem_addr == MAP_FAILED) {
        perror("Error mapping shared memory");
        return -1;
    }

    // Optionally store the shared memory address in a global or static variable if needed elsewhere
    // For example:
    // global_shmem_addr = shmem_addr;

    return 0;
}


int mf_disconnect() {
    // Assuming you have stored these as global or static to access here
    extern void *global_shmem_addr;  // Pointer to the shared memory
    extern int global_shmem_size;    // Size of the shared memory
    extern int shm_fd;               // File descriptor for the shared memory

    // Unmap the shared memory
    if (munmap(global_shmem_addr, global_shmem_size) == -1) {
        perror("Error unmapping shared memory");
        return -1;
    }

    // Close the shared memory file descriptor
    if (close(shm_fd) == -1) {
        perror("Error closing shared memory descriptor");
        return -1;
    }

    // Optionally, here you would also decrement a count of connected processes
    // and only call mf_destroy() if it's the last one.
    // This part depends on whether you are keeping track of active processes.

    return 0;
}


int mf_create(char *mqname, int mqsize) {
    // Acquire the global semaphore to ensure exclusive access to queue metadata
    sem_wait(semaphore_id);

    // Check if there's enough space in shared memory for a new queue
    if (shmem_metadata->num_queues >= max_queues_in_shmem) {
        sem_post(semaphore_id);
        return -1; // No more space for new queues
    }

    // Calculate the offset for the new queue
    size_t offset = sizeof(shmem_metadata_t) + (shmem_metadata->num_queues * sizeof(mf_queue_t));

    if (offset + sizeof(mf_queue_t) + mqsize * MAX_DATALEN > shmem_size) {
        sem_post(semaphore_id);
        return -1; // Not enough space in shared memory
    }

    // Create a new queue
    mf_queue_t *new_queue = (mf_queue_t *)((char *)shmem_addr + offset);
    strncpy(new_queue->name, mqname, MAX_MQNAMESIZE - 1);
    new_queue->name[MAX_MQNAMESIZE - 1] = '\0';
    new_queue->size = mqsize * MAX_DATALEN; // Size in bytes
    new_queue->in = 0;
    new_queue->out = 0;
    new_queue->ref_count = 0;

    // Initialize semaphores for the new queue
    char sem_name[256];
    snprintf(sem_name, sizeof(sem_name), "enqueue_%s", mqname);
    new_queue->sem_enqueue = sem_open(sem_name, O_CREAT, 0644, mqsize); // Assuming mqsize is the initial semaphore value
    snprintf(sem_name, sizeof(sem_name), "dequeue_%s", mqname);
    new_queue->sem_dequeue = sem_open(sem_name, O_CREAT, 0644, 0);
    snprintf(sem_name, sizeof(sem_name), "mutex_%s", mqname);
    new_queue->mutex = sem_open(sem_name, O_CREAT, 0644, 1);

    // Check semaphore initialization success
    if (new_queue->sem_enqueue == SEM_FAILED || new_queue->sem_dequeue == SEM_FAILED || new_queue->mutex == SEM_FAILED) {
        sem_post(semaphore_id);
        return -1; // Semaphore initialization failed
    }

    // Increment the number of queues
    shmem_metadata->num_queues++;

    // Release the global semaphore
    sem_post(semaphore_id);

    return 0;
}


int mf_remove(char *mqname) {
    // Acquire the semaphore
    sem_wait(semaphore_id);

    // Find the queue to remove
    mf_queue_t *queue_to_remove = NULL;
    size_t offset = sizeof(shmem_metadata_t);
    for (int i = 0; i < shmem_metadata->num_queues; i++) {
        mf_queue_t *queue = (mf_queue_t *)((char *)shmem_addr + offset);
        if (strcmp(queue->name, mqname) == 0) {
            queue_to_remove = queue;
            break;
        }
        offset += sizeof(mf_queue_t);
    }

    // If the queue is not found, return an error
    if (queue_to_remove == NULL) {
        sem_post(semaphore_id);
        return -1;
    }

    // Check if the queue is still in use (reference count > 0)
    // Implement the reference count checking logic here

    // Remove the queue by shifting the remaining queues in memory
    size_t remaining_bytes = (char *)shmem_addr + shmem_size - (char *)queue_to_remove - sizeof(mf_queue_t);
    memmove(queue_to_remove, (char *)queue_to_remove + sizeof(mf_queue_t), remaining_bytes);

    // Decrement the number of queues
    shmem_metadata->num_queues--;

    // Release the semaphore
    sem_post(semaphore_id);

    return 0;
}

int mf_open(char *mqname) {
    // Acquire the semaphore
    sem_wait(semaphore_id);

    // Find the queue with the given name
    mf_queue_t *queue = NULL;
    size_t offset = sizeof(shmem_metadata_t);
    for (int i = 0; i < shmem_metadata->num_queues; i++) {
        queue = (mf_queue_t *)((char *)shmem_addr + offset);
        if (strcmp(queue->name, mqname) == 0) {
            break;
        }
        offset += sizeof(mf_queue_t);
    }

    // If the queue is not found, return an error
    if (queue == NULL) {
        sem_post(semaphore_id);
        return -1;
    }

    // Increment the reference count for the queue
    // Implement the reference count incrementing logic here

    // Release the semaphore
    sem_post(semaphore_id);

    // Return the queue ID (index of the queue in shared memory)
    return (int)(offset - sizeof(shmem_metadata_t)) / sizeof(mf_queue_t);
}

int mf_close(int qid) {
    // Acquire the semaphore
    sem_wait(semaphore_id);

    // Calculate the offset of the queue based on the queue ID
    size_t offset = sizeof(shmem_metadata_t) + qid * sizeof(mf_queue_t);

    // Check if the queue ID is valid
    if (offset >= shmem_size || offset < sizeof(shmem_metadata_t)) {
        sem_post(semaphore_id);
        return -1;
    }

    mf_queue_t *queue = (mf_queue_t *)((char *)shmem_addr + offset);

    // Decrement the reference count for the queue
    // Implement the reference count decrementing logic here

    // Release the semaphore
    sem_post(semaphore_id);

    return 0;
}



int mf_send(int qid, void *bufptr, int datalen) {
    if (qid < 0 || qid >= shmem_metadata->num_queues) {
        return -1;  // Invalid queue ID
    }

    // Calculate the actual queue address based on qid
    mf_queue_t *queue = (mf_queue_t *)((char *)shmem_addr + sizeof(shmem_metadata_t) + qid * sizeof(mf_queue_t));

    // Wait for space to become available in the queue
    sem_wait(queue->sem_enqueue);  // Corrected

    // Acquire mutex to ensure exclusive access to queue modifications
    sem_wait(queue->mutex);  // Corrected

    // Check if there is enough space to add the new message
    int next_in = (queue->in + datalen) % queue->size;
    if (datalen > queue->size || ((queue->in < queue->out) && (next_in >= queue->out)) ||
        ((queue->in > queue->out) && (next_in > queue->in && next_in < queue->out))) {
        // Not enough space, need to handle this situation appropriately
        sem_post(queue->mutex);  // Corrected
        sem_post(queue->sem_enqueue);  // Corrected, Release the enqueue semaphore since we are not enqueueing
        return -1;
    }

    // Copy the message data into the buffer
    memcpy(queue->buffer + queue->in, bufptr, datalen);

    // Update the input index and possibly wrap around
    queue->in = next_in;

    // Release the mutex
    sem_post(queue->mutex);  // Corrected

    // Signal that there is a new item in the queue
    sem_post(queue->sem_dequeue);  // Corrected

    return 0;
}


int mf_recv(int qid, void *bufptr, int bufsize) {
    if (qid < 0 || qid >= shmem_metadata->num_queues) {
        return -1;  // Invalid queue ID
    }

    // Calculate the actual queue address based on qid
    mf_queue_t *queue = (mf_queue_t *)((char *)shmem_addr + sizeof(shmem_metadata_t) + qid * sizeof(mf_queue_t));

    // Wait for a message to become available
    sem_wait(queue->sem_dequeue);

    // Acquire mutex to ensure exclusive access to queue modifications
    sem_wait(queue->mutex);

    // Determine the size of the next message
    int msg_len;
    // Assuming message length is stored at the position 'out'
    memcpy(&msg_len, queue->buffer + queue->out, sizeof(int));

    // Check if the provided buffer size is sufficient
    if (bufsize < msg_len) {
        sem_post(queue->mutex); // Release mutex
        sem_post(queue->sem_dequeue); // Message still in queue, re-signal it
        return -1;
    }

    // Calculate the start position of the message data in the buffer
    int data_pos = (queue->out + sizeof(int)) % queue->size;

    // Copy the message data from the buffer
    if (data_pos + msg_len <= queue->size) {
        memcpy(bufptr, queue->buffer + data_pos, msg_len);
    } else {
        // Handle wrap-around case
        int first_part_size = queue->size - data_pos;
        memcpy(bufptr, queue->buffer + data_pos, first_part_size);
        memcpy((char*)bufptr + first_part_size, queue->buffer, msg_len - first_part_size);
    }

    // Update the out index and wrap around if necessary
    queue->out = (queue->out + sizeof(int) + msg_len) % queue->size;

    // Release the mutex
    sem_post(queue->mutex);

    // Signal that there is now more space available in the queue
    sem_post(queue->sem_enqueue);

    return msg_len;
}


int mf_print()
{
    return (0);
}


