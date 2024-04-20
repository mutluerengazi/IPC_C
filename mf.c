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
static char shmem_name[MAXFILENAME] = "";  // initialized to empty string
static int shmem_size = 0;
static int max_msgs_in_queue;
static int max_queues_in_shmem;
static void *shmem_addr = NULL;  // pointer initialization to NULL
static sem_t *semaphore_id = NULL; 

void *global_shmem_addr = NULL;  // Initialize pointer to NULL
int global_shmem_size = 0;       // Initialize size to 0
int shm_fd = -1;                 // Initialize file descriptor to invalid value


shmem_metadata_t *shmem_metadata;

int mf_init() {
    FILE *config_file = fopen(CONFIG_FILENAME, "r");
    if (config_file == NULL) {
        perror("Error opening config file");
        return -1;
    }

    char line[256], key[256], value[256];
    char local_shmem_name[256];  // Use a local variable to avoid global conflicts
    int local_shmem_size = 0;    // Local variable to hold the shared memory size
    int local_max_queues = 0;    // Local variable to hold the max number of queues

    // Reading configuration parameters
    while (fgets(line, sizeof(line), config_file)) {
        if (line[0] == '#' || isspace(line[0])) continue;  // Ignore comments and blank lines
        
        if (sscanf(line, "%s %s", key, value) != 2) {
            fprintf(stderr, "Malformed line in config file: %s", line);
            continue;  // Skip malformed lines
        }

        if (strcmp(key, "SHMEM_NAME") == 0) {
            strncpy(local_shmem_name, value, sizeof(local_shmem_name) - 1);
            local_shmem_name[sizeof(local_shmem_name) - 1] = '\0';
        } else if (strcmp(key, "SHMEM_SIZE") == 0) {
            local_shmem_size = atoi(value) * 1024;  // Convert KB to bytes
        } else if (strcmp(key, "MAX_QUEUES_IN_SHMEM") == 0) {
            local_max_queues = atoi(value);  // Read maximum number of queues
        }
    }
    fclose(config_file);

    // Ensure all necessary configuration parameters were successfully read
    if (local_shmem_name[0] == '\0' || local_shmem_size <= 0 || local_max_queues <= 0) {
        fprintf(stderr, "Configuration incomplete or invalid.\n");
        return -1;
    }

    // Create or open the shared memory object
    shm_fd = shm_open(local_shmem_name, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("Error creating or accessing shared memory");
        return -1;
    }

    // Set the size of the shared memory object
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

    // Initialize semaphores for each queue
    char sem_name[256];
    for (int i = 0; i < max_queues_in_shmem; i++) {
        sprintf(sem_name, "/sem_enqueue_%d", i);
        if (sem_open(sem_name, O_CREAT, 0644, 1) == SEM_FAILED) {
            perror("Failed to create enqueue semaphore");
            return -1;  // Handle failure: cleanup and exit
        }

        sprintf(sem_name, "/sem_dequeue_%d", i);
        if (sem_open(sem_name, O_CREAT, 0644, 0) == SEM_FAILED) {
            perror("Failed to create dequeue semaphore");
            return -1;  // Handle failure: cleanup and exit
        }

        sprintf(sem_name, "/sem_mutex_%d", i);
        if (sem_open(sem_name, O_CREAT, 0644, 1) == SEM_FAILED) {
            perror("Failed to create mutex semaphore");
            return -1;  // Handle failure: cleanup and exit
        }
    }

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
    printf("mf connect starts..\n");

    FILE *config_file = fopen(CONFIG_FILENAME, "r");
    if (config_file == NULL) {
        perror("Error opening config file");
        return -1;
    }

    char line[256], key[256], value[256];
    char local_shmem_name[256];  // Local variable to hold the shared memory name
    int local_shmem_size = 0;    // Local variable to hold the shared memory size

    // Read configuration settings
    while (fgets(line, sizeof(line), config_file)) {
        if (line[0] == '#' || isspace(line[0])) continue; // Skip comments and blank lines
        
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
    // Ensure that the configuration parameters were successfully read
    if (local_shmem_name[0] == '\0' || local_shmem_size <= 0) {
        fprintf(stderr, "Configuration incomplete or invalid.\n");
        return -1;
    }

    // Attach to the existing shared memory segment
    int shm_fd = shm_open(local_shmem_name, O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("Error accessing shared memory");
        return -1;
    }

    void *temp_addr = mmap(NULL, local_shmem_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (temp_addr == MAP_FAILED) {
        perror("Error mapping shared memory");
        close(shm_fd);  // Properly close the file descriptor if mmap fails
        return -1;
    }

    // Correctly update global address and size
    global_shmem_addr = temp_addr;
    global_shmem_size = local_shmem_size;  // Ensure the global size is updated to reflect the actual mapped size

    shmem_addr = global_shmem_addr;  // Update the local pointer for internal use
    shmem_size = global_shmem_size;  // Update the local size for internal use

    // Initialize metadata pointer
    shmem_metadata = (shmem_metadata_t *)global_shmem_addr;

    // It's good practice to close the file descriptor after successful mmap
    close(shm_fd);

    printf("MFCONNECT Shared Memory Address: %p, Size: %d\n", global_shmem_addr, global_shmem_size);
    printf("mf connect ends..\n");
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
    printf ("mf create starts..\n");
    // Acquire the global semaphore to ensure exclusive access to queue metadata
    // Initialization of shared memory and shmem_metadata
    if (shmem_addr == MAP_FAILED) {
        perror("Error mapping shared memory");
        shm_unlink(shmem_name); // Cleanup on failure
        return -1;
    }
    if (shmem_metadata == NULL) {
        fprintf(stderr, "Failed to allocate metadata\n");
        return -1;
    }
    shmem_metadata->num_queues = 0; // Proper initialization

    printf ("mf create sem wait..\n");
    
    sem_wait(semaphore_id);
    printf ("mf create sem ends..\n");

    if (shmem_metadata == NULL) {
        fprintf(stderr, "shmem_metadata is not initialized\n");
        sem_post(semaphore_id);
        return -1;
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
    new_queue->sem_dequeue = sem_open(sem_name, O_CREAT, 0644, mqsize);
    snprintf(sem_name, sizeof(sem_name), "mutex_%s", mqname);
    new_queue->mutex = sem_open(sem_name, O_CREAT, 0644, 1);

    // Check semaphore initialization success
    if (new_queue->sem_enqueue == SEM_FAILED || new_queue->sem_dequeue == SEM_FAILED || new_queue->mutex == SEM_FAILED) {
        sem_post(semaphore_id);
        return -1; // Semaphore initialization failed
    }

    // Increment the number of queues
     shmem_metadata->num_queues++;
    printf("mf create shmem_metadata->num_queues= %d \n",shmem_metadata->num_queues);
    // Release the global semaphore
    sem_post(semaphore_id);
    printf ("mf create ends..\n");
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
    if (qid < 0 || qid >= shmem_metadata->num_queues || datalen <= 0 || datalen > MAX_DATALEN) {
        return -1;  // Validate queue ID and data length
    }
   
    printf("mf_send started\n");

    mf_queue_t *queue = (mf_queue_t *)((char *)shmem_addr + sizeof(shmem_metadata_t) + qid * sizeof(mf_queue_t));

    sem_wait(queue->sem_enqueue);  // Wait for space to become available in the queue
    printf("mf_send passed sem_enqueue\n");
    sem_wait(queue->mutex);  // Acquire mutex to ensure exclusive access to queue modifications
   

    // Calculate total data length including the size prefix and check for space availability
    int total_data_len = datalen + sizeof(int); // Total length including the size prefix
    int next_in = (queue->in + total_data_len) % queue->size;
    if (total_data_len > queue->size || 
       ((queue->in < queue->out) && (next_in >= queue->out && next_in < queue->in)) ||
       ((queue->in >= queue->out) && (next_in < queue->in && queue->out <= next_in))) {
        sem_post(queue->mutex);  // Release mutex
        sem_post(queue->sem_enqueue);  // Release the enqueue semaphore since we are not enqueueing
        return -1;
    }

    // Store the message length at the current 'in' position
    memcpy(queue->buffer + queue->in, &datalen, sizeof(int));
    queue->in = (queue->in + sizeof(int)) % queue->size;

    // Store the message data, handling potential wrap-around
    int first_part_size = min(datalen, queue->size - queue->in);
    memcpy(queue->buffer + queue->in, bufptr, first_part_size);
    if (datalen > first_part_size) {  // Handle wrap-around
        memcpy(queue->buffer, (char *)bufptr + first_part_size, datalen - first_part_size);
    }
    queue->in = next_in;

    sem_post(queue->mutex);  // Release mutex
    sem_post(queue->sem_dequeue);  // Signal that a new item is available in the queue

    return 0;
}





int mf_recv(int qid, void *bufptr, int bufsize) {
    printf("Receiving with qid: %d, num_queues: %d, bufsize: %d\n", qid, shmem_metadata->num_queues, bufsize);
    if (qid < 0) {
        printf("Error: Invalid queue ID, qid < 0\n");
        return -1;
    } else if (bufsize < MIN_DATALEN) {
        printf("Error: Buffer size too small, bufsize < MIN_DATALEN (%d < %d)\n", bufsize, MIN_DATALEN);
        return -1;
    }

    mf_queue_t *queue = (mf_queue_t *)((char *)shmem_addr + sizeof(shmem_metadata_t) + qid * sizeof(mf_queue_t));

    sem_wait(queue->sem_dequeue);  // Wait until there's a message to receive
    sem_wait(queue->mutex);  // Lock the queue for exclusive access

    // Read the message length safely considering wrap-around at the buffer end
    int msg_len;
    if (queue->out + sizeof(int) > queue->size) {  // Handle wrap-around of the size field
        int first_part_size = queue->size - queue->out;
        memcpy(&msg_len, queue->buffer + queue->out, first_part_size);
        memcpy((char *)&msg_len + first_part_size, queue->buffer, sizeof(int) - first_part_size);
        queue->out = (sizeof(int) - first_part_size) % queue->size;
    } else {
        memcpy(&msg_len, queue->buffer + queue->out, sizeof(int));
        queue->out = (queue->out + sizeof(int)) % queue->size;
    }

    if (bufsize < msg_len) {  // Ensure the user buffer is large enough
        printf("second error at if check \n");
        sem_post(queue->mutex);
        sem_post(queue->sem_dequeue);  // Re-signal dequeue since message is not read
        return -1;
    }

    // Read the message data considering potential wrap-around
    if (queue->out + msg_len > queue->size) {  // Handle wrap-around of the data
        int first_part_size = queue->size - queue->out;
        memcpy(bufptr, queue->buffer + queue->out, first_part_size);
        memcpy((char *)bufptr + first_part_size, queue->buffer, msg_len - first_part_size);
        queue->out = (msg_len - first_part_size) % queue->size;
    } else {
        memcpy(bufptr, queue->buffer + queue->out, msg_len);
        queue->out = (queue->out + msg_len) % queue->size;
    }

    sem_post(queue->mutex);
    sem_post(queue->sem_enqueue);  // Signal space available for new messages

    return msg_len;
}




int mf_print()
{
    return (0);
}


