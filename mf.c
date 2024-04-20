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
#include <ctype.h> 

// Global variables to store configuration and shared memory information
static char shmem_name[MAXFILENAME] = "";  // initialized to empty string
static int shmem_size = 0;
static int max_msgs_in_queue;
static int max_queues_in_shmem;
static void *shmem_addr = NULL;  // pointer initialization to NULL
static sem_t *semaphore_id = NULL; 

void *global_shmem_addr = NULL;  // initialize pointer to NULL
int global_shmem_size = 0;       // initialize size to 0
int shm_fd = -1;                 // initialize file descriptor to invalid value


shmem_metadata_t *shmem_metadata;

int mf_init() {
    FILE *config_file = fopen(CONFIG_FILENAME, "r");
    if (config_file == NULL) {
        perror("Error opening config file");
        return -1;
    }

    char line[256], key[256], value[256];
    char local_shmem_name[256];  // use a local variable to avoid global conflicts
    int local_shmem_size = 0;    // local variable to hold the shared memory size
    int local_max_queues = 0;    // local variable to hold the max number of queues

    // reading configuration parameters
    while (fgets(line, sizeof(line), config_file)) {
        if (line[0] == '#' || isspace(line[0])) continue;  // ignore comments and blank lines
        
        if (sscanf(line, "%s %s", key, value) != 2) {
            fprintf(stderr, "Malformed line in config file: %s", line);
            continue;  
        }

        if (strcmp(key, "SHMEM_NAME") == 0) {
            strncpy(local_shmem_name, value, sizeof(local_shmem_name) - 1);
            local_shmem_name[sizeof(local_shmem_name) - 1] = '\0';
        } else if (strcmp(key, "SHMEM_SIZE") == 0) {
            local_shmem_size = atoi(value) * 1024;  // convert KB to bytes
        } else if (strcmp(key, "MAX_QUEUES_IN_SHMEM") == 0) {
            local_max_queues = atoi(value);  // read maximum number of queues
        }
    }
    fclose(config_file);

    // ensure all necessary configuration parameters were successfully read
    if (local_shmem_name[0] == '\0' || local_shmem_size <= 0 || local_max_queues <= 0) {
        fprintf(stderr, "Configuration incomplete or invalid.\n");
        return -1;
    }

    // create or open the shared memory object
    shm_fd = shm_open(local_shmem_name, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("Error creating or accessing shared memory");
        return -1;
    }

    // set the size of the shared memory object
    if (ftruncate(shm_fd, local_shmem_size) == -1) {
        perror("Error setting shared memory size");
        shm_unlink(local_shmem_name);  // Cleanup on failure
        close(shm_fd);
        return -1;
    }

    // Map the shared memory object
    global_shmem_addr = mmap(NULL, local_shmem_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (global_shmem_addr == MAP_FAILED) {
        perror("Error mapping shared memory");
        shm_unlink(local_shmem_name);  // Cleanup on failure
        close(shm_fd);
        return -1;
    }

    global_shmem_size = local_shmem_size;  // Store the size globally
    strcpy(shmem_name, local_shmem_name);  // Store the name globally
    shmem_size = local_shmem_size;         // Store the size globally
    shmem_addr = global_shmem_addr;        // Align local pointer to global pointer
    max_queues_in_shmem = local_max_queues; // Store the maximum number of queues globally

    // Setup the metadata structure at the beginning of the shared memory
    shmem_metadata = (shmem_metadata_t *)global_shmem_addr;
    shmem_metadata->num_queues = 0;  // Initialize current queue count to 0

    // Print debugging information
    printf("mf_init: Shared Memory Address: %p, Size: %d, Max Queues: %d\n", global_shmem_addr, global_shmem_size, max_queues_in_shmem);

    // Initialize global semaphore for synchronization
    semaphore_id = sem_open("/global_semaphore", O_CREAT, 0644, 1);
    if (semaphore_id == SEM_FAILED) {
        perror("Error opening global semaphore");
        munmap(global_shmem_addr, local_shmem_size);
        shm_unlink(local_shmem_name);
        close(shm_fd);
        return -1;
    }
    sem_post(semaphore_id);
    return 0;  // Success
}



int mf_destroy() {
    int cleanup_status = 0;

    // unlink global named semaphore
    if (sem_unlink("/global_semaphore") == -1) {
        perror("Error unlinking mutex semaphore");
        cleanup_status = -1;
    }
    // unmap the shared memory
    if (munmap(shmem_addr, shmem_size) == -1) {
        perror("Error unmapping shared memory");
        cleanup_status = -1;
    }

    // remove the shared memory segment
    if (shm_unlink(shmem_name) == -1) {
        perror("Error removing shared memory");
        cleanup_status = -1;
    }

    return cleanup_status;
}

int mf_connect() {
    printf("mf connect starts..\n");

    FILE *config_file = fopen(CONFIG_FILENAME, "r");
    if (config_file == NULL) {
        perror("Error opening config file");
        return -1;
    }

    char line[256], key[256], value[256];
    char local_shmem_name[256];  // local variable to hold the shared memory name
    int local_shmem_size = 0;    // local variable to hold the shared memory size

    // read configuration settings
    while (fgets(line, sizeof(line), config_file)) {
        if (line[0] == '#' || isspace(line[0])) continue; // skip comments and blank lines
        
        if (sscanf(line, "%s %s", key, value) != 2) {
            fprintf(stderr, "Malformed line in config file: %s", line);
            continue; // Skip malformed lines
        }

        if (strcmp(key, "SHMEM_NAME") == 0) {
            strncpy(local_shmem_name, value, sizeof(local_shmem_name) - 1);
            local_shmem_name[sizeof(local_shmem_name) - 1] = '\0';
        } else if (strcmp(key, "SHMEM_SIZE") == 0) {
            local_shmem_size = atoi(value) * 1024; // Convert KB to bytes
        }
    }
    fclose(config_file);
    semaphore_id = sem_open("/global_semaphore", O_CREAT, 0644, 1);
    // ensure that the configuration parameters were successfully read
    if (local_shmem_name[0] == '\0' || local_shmem_size <= 0) {
        fprintf(stderr, "Configuration incomplete or invalid.\n");
        return -1;
    }

    // attach to the existing shared memory segment
    int shm_fd = shm_open(local_shmem_name, O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("Error accessing shared memory");
        return -1;
    }

    void *temp_addr = mmap(NULL, local_shmem_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (temp_addr == MAP_FAILED) {
        perror("Error mapping shared memory");
        close(shm_fd);  // properly close the file descriptor if mmap fails
        return -1;
    }

    // correctly update global address and size
    global_shmem_addr = temp_addr;
    global_shmem_size = local_shmem_size;  // ensure the global size is updated to reflect the actual mapped size

    shmem_addr = global_shmem_addr;  // update the local pointer for internal use
    shmem_size = global_shmem_size;  // update the local size for internal use

    // initialize metadata pointer
    shmem_metadata = (shmem_metadata_t *)global_shmem_addr;

    // close the file descriptor after successful mmap
    close(shm_fd);

    printf("MFCONNECT Shared Memory Address: %p, Size: %d\n", global_shmem_addr, global_shmem_size);
    printf("mf connect ends..\n");
    return 0;
}

int mf_disconnect() {
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
    shmem_metadata->num_queues--;
    return 0;
}

int mf_create(char *mqname, int mqsize) {
    printf("mf create starts..\n");

    // acquire the global semaphore to ensure access to the shared memory
    sem_wait(semaphore_id);

    // validation of mqsize (converted from KB to bytes for internal calculation)
    int buffer_size = mqsize * 1024; // mqsize specified in KB, converted to bytes
    if (mqsize < MIN_MQSIZE || mqsize > MAX_MQSIZE || buffer_size % 4096 != 0) {
        fprintf(stderr, "Queue size %d KB is out of bounds or not a multiple of 4KB.\n", mqsize);
        sem_post(semaphore_id);
        return -1;
    }

    // check if there is enough space left in the shared memory
    size_t offset = sizeof(shmem_metadata_t) + (shmem_metadata->num_queues * sizeof(mf_queue_t));
    if (offset + sizeof(mf_queue_t) + buffer_size > global_shmem_size) {
        fprintf(stderr, "Not enough space in shared memory to create a new message queue.\n");
        sem_post(semaphore_id);
        return -1;
    }

    // setup the new queue at the calculated offset
    mf_queue_t *new_queue = (mf_queue_t *)((char *)global_shmem_addr + offset);
    strncpy(new_queue->name, mqname, MAX_MQNAMESIZE - 1);
    new_queue->name[MAX_MQNAMESIZE - 1] = '\0';  // Ensure null termination
    new_queue->size = buffer_size;
    new_queue->in = 0;
    new_queue->out = 0;
    new_queue->ref_count = 0;

    // Increment the number of queues
    shmem_metadata->num_queues++;
    printf("mf create: Queue created successfully. Total queues: %d\n", shmem_metadata->num_queues);

    sem_post(semaphore_id);  // Release the global semaphore
    return 0;  // Success
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
    printf("mf open starts... \n");
    sem_wait(semaphore_id);  // Synchronize access
    printf("mf open shmem_metadata->num_queues= %d \n",shmem_metadata->num_queues);

    for (int i = 0; i < shmem_metadata->num_queues; i++) {
        printf("mf open for... \n");
        mf_queue_t *queue = (mf_queue_t *)((char *)shmem_addr + sizeof(shmem_metadata_t) + i * sizeof(mf_queue_t));
        if (strcmp(queue->name, mqname) == 0) {
            sem_post(semaphore_id);
            printf("open qid: %d \n", i + 1);
            return i + 1;  // Return the index (qid) of the found queue
        }
    }
    printf("mf open starts... \n");
    sem_post(semaphore_id);
    return -1;  // Queue not found
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
    if (datalen > MAX_DATALEN || datalen <= 0) {
        fprintf(stderr, "Invalid data length.\n");
        return -1;
    }

    sem_wait(semaphore_id); // Lock the global semaphore

    mf_queue_t *queue = (mf_queue_t *)((char *)global_shmem_addr + sizeof(shmem_metadata_t) + (qid - 1) * sizeof(mf_queue_t));
    int total_size = datalen + sizeof(int);  // Total size to store length + data
    int next_in = (queue->in + total_size) % queue->size;

    if ((next_in == queue->out) || (queue->size - queue->in < total_size && queue->out < queue->in)) {
        sem_post(semaphore_id);
        printf("space exceeded!\n");
        return -1; // Not enough space
    }

    // Store message length first
    char *queue_buffer = (char *)queue + sizeof(mf_queue_t);
    if (queue->in + sizeof(int) > queue->size) {  // Check wrap-around for length
        memcpy(queue_buffer + queue->in, &datalen, queue->size - queue->in);  // Part before wrap
        memcpy(queue_buffer, ((char*)&datalen) + (queue->size - queue->in), sizeof(int) - (queue->size - queue->in));  // Part after wrap
        queue->in = sizeof(int) - (queue->size - queue->in);
    } else {
        memcpy(queue_buffer + queue->in, &datalen, sizeof(int));
        queue->in = (queue->in + sizeof(int)) % queue->size;
    }

    // Store message data
    if (queue->in + datalen > queue->size) {  // Check wrap-around for data
        memcpy(queue_buffer + queue->in, bufptr, queue->size - queue->in);  // Part before wrap
        memcpy(queue_buffer, ((char*)bufptr) + (queue->size - queue->in), datalen - (queue->size - queue->in));  // Part after wrap
        queue->in = datalen - (queue->size - queue->in);
    } else {
        memcpy(queue_buffer + queue->in, bufptr, datalen);
        queue->in = (queue->in + datalen) % queue->size;
    }

    sem_post(semaphore_id); // Release the global semaphore
    return 0;
}
int mf_recv(int qid, void *bufptr, int bufsize) {
    sem_wait(semaphore_id);  // Synchronize access

    mf_queue_t *queue = (mf_queue_t *)((char *)global_shmem_addr + sizeof(shmem_metadata_t) + (qid - 1) * sizeof(mf_queue_t));
    if (queue->out == queue->in) {
        sem_post(semaphore_id);
        return -1;  // Queue empty, nothing to receive
    }

    // Read the length of the next message
    int msg_len;
    char *queue_buffer = (char *)queue + sizeof(mf_queue_t);
    if (queue->out + sizeof(int) > queue->size) {  // Wrap-around case for length
        int first_part = queue->size - queue->out;
        memcpy(&msg_len, queue_buffer + queue->out, first_part);  // Part before wrap
        memcpy(((char*)&msg_len) + first_part, queue_buffer, sizeof(int) - first_part);  // Part after wrap
    } else {
        memcpy(&msg_len, queue_buffer + queue->out, sizeof(int));
    }

    int start_data = (queue->out + sizeof(int)) % queue->size;  // Start of message data
    if (start_data + msg_len > queue->size) {  // Handle wrap-around
        int first_part_size = queue->size - start_data;
        memcpy(bufptr, queue_buffer + start_data, first_part_size);
        memcpy((char *)bufptr + first_part_size, queue_buffer, msg_len - first_part_size);
    } else {
        memcpy(bufptr, queue_buffer + start_data, msg_len);
    }

    queue->out = (start_data + msg_len) % queue->size;  // Move the out pointer past the message

    sem_post(semaphore_id);
    return msg_len;  // Return the length of the message received
}

int mf_print()
{
    return (0);
}


