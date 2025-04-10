#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <errno.h>
#include "shared_memory.h"

// Global variables
static int g_semid = -1;

// Initialize shared memory for tab synchronization
int init_shared_memory() {
    int shmid = shmget(SHM_KEY, sizeof(SharedState), IPC_CREAT | 0666);
    if (shmid < 0) {
        perror("shmget");
        return -1;
    }
    
    // Attach to shared memory
    SharedState *state = (SharedState *)shmat(shmid, NULL, 0);
    if (state == (void *) -1) {
        perror("shmat");
        return -1;
    }
    
    // Initialize shared memory if we're the first to create it
    if (shmid != 0) {
        memset(state, 0, sizeof(SharedState));
        state->last_activity = time(NULL);
        printf("[Shared Memory] Initialized with ID: %d\n", shmid);
    }
    
    // Detach from shared memory
    shmdt(state);
    return shmid;
}

// Initialize semaphores for synchronization
int init_semaphores() {
    // Create semaphore
    g_semid = semget(SEM_KEY, 1, IPC_CREAT | 0666);
    if (g_semid < 0) {
        perror("semget");
        return -1;
    }
    
    // Initialize semaphore to 1 (unlocked)
    union semun arg;
    arg.val = 1;
    if (semctl(g_semid, 0, SETVAL, arg) < 0) {
        perror("semctl");
        return -1;
    }
    
    printf("[Semaphore] Initialized with ID: %d\n", g_semid);
    return g_semid;
}

// Lock shared memory using semaphore
void lock_shared_memory() {
    if (g_semid < 0) return;
    
    struct sembuf sb;
    sb.sem_num = 0;
    sb.sem_op = -1;  // Decrement by 1 (lock)
    sb.sem_flg = SEM_UNDO;
    
    if (semop(g_semid, &sb, 1) < 0) {
        perror("semop: lock");
    }
}

// Unlock shared memory using semaphore
void unlock_shared_memory() {
    if (g_semid < 0) return;
    
    struct sembuf sb;
    sb.sem_num = 0;
    sb.sem_op = 1;  // Increment by 1 (unlock)
    sb.sem_flg = SEM_UNDO;
    
    if (semop(g_semid, &sb, 1) < 0) {
        perror("semop: unlock");
    }
}

// Attach to shared memory
void *attach_shared_memory(int shmid) {
    SharedState *state = (SharedState *)shmat(shmid, NULL, 0);
    if (state == (void *) -1) {
        perror("shmat");
        return NULL;
    }
    return state;
}

// Detach from shared memory
void detach_shared_memory(void *segment) {
    if (segment != NULL) {
        shmdt(segment);
    }
}

// Send broadcast message to all tabs
void broadcast_message(SharedState *state, BroadcastType type, int sender_tab_id, const char *data) {
    if (!state) return;
    
    lock_shared_memory();
    
    // Find an available slot or reuse oldest
    int slot = state->broadcast_count % 10;
    
    // Prepare broadcast message
    BroadcastMessage *msg = &state->broadcast_messages[slot];
    msg->type = type;
    msg->sender_tab_id = sender_tab_id;
    msg->timestamp = time(NULL);
    strncpy(msg->data, data, BROADCAST_MSG_SIZE - 1);
    msg->data[BROADCAST_MSG_SIZE - 1] = '\0';
    
    // Mark as unprocessed for all tabs
    for (int i = 0; i < MAX_TABS; i++) {
        msg->processed[i] = (i == sender_tab_id) ? true : false;
    }
    
    // Update broadcast count
    state->broadcast_count++;
    state->last_activity = time(NULL);
    
    unlock_shared_memory();
    
    printf("[Broadcast] Tab %d sent message type %d: %s\n", 
           sender_tab_id, type, data);
}

// Check if there are new broadcasts for this tab
bool check_new_broadcasts(SharedState *state, int tab_id) {
    if (!state) return false;
    
    lock_shared_memory();
    
    bool has_new = false;
    for (int i = 0; i < 10; i++) {
        BroadcastMessage *msg = &state->broadcast_messages[i];
        if (msg->timestamp > 0 && !msg->processed[tab_id]) {
            has_new = true;
            break;
        }
    }
    
    unlock_shared_memory();
    return has_new;
}

// Process all pending broadcasts for this tab
void process_broadcasts(SharedState *state, int tab_id) {
    if (!state) return;
    
    lock_shared_memory();
    
    for (int i = 0; i < 10; i++) {
        BroadcastMessage *msg = &state->broadcast_messages[i];
        
        if (msg->timestamp > 0 && !msg->processed[tab_id]) {
            // Mark as processed
            msg->processed[tab_id] = true;
            
            // Process based on type
            switch (msg->type) {
                case BROADCAST_BOOKMARK_ADDED:
                    printf("[Tab %d] Received: Bookmark added by Tab %d: %s\n", 
                           tab_id, msg->sender_tab_id, msg->data);
                    break;
                    
                case BROADCAST_BOOKMARK_REMOVED:
                    printf("[Tab %d] Received: Bookmark removed by Tab %d: %s\n", 
                           tab_id, msg->sender_tab_id, msg->data);
                    break;
                    
                case BROADCAST_NEW_TAB:
                    printf("[Tab %d] Received: New tab opened: %d\n", 
                           tab_id, msg->sender_tab_id);
                    break;
                    
                case BROADCAST_TAB_CLOSED:
                    printf("[Tab %d] Received: Tab closed: %d\n", 
                           tab_id, msg->sender_tab_id);
                    break;
                    
                case BROADCAST_PAGE_LOADED:
                    printf("[Tab %d] Received: Tab %d loaded page: %s\n", 
                           tab_id, msg->sender_tab_id, msg->data);
                    break;
            }
        }
    }
    
    unlock_shared_memory();
}

// Add a bookmark
void add_bookmark(SharedState *state, const char *url, const char *title, int sender_tab_id) {
    if (!state) return;
    
    lock_shared_memory();
    
    if (state->bookmark_count < MAX_BOOKMARKS) {
        Bookmark *bookmark = &state->bookmarks[state->bookmark_count];
        strncpy(bookmark->url, url, MAX_URL_LENGTH - 1);
        strncpy(bookmark->title, title, MAX_URL_LENGTH - 1);
        bookmark->is_active = true;
        state->bookmark_count++;
        
        // Broadcast to other tabs
        char message[BROADCAST_MSG_SIZE];
        snprintf(message, BROADCAST_MSG_SIZE, "%s (%s)", title, url);
        
        unlock_shared_memory();
        
        broadcast_message(state, BROADCAST_BOOKMARK_ADDED, sender_tab_id, message);
    } else {
        unlock_shared_memory();
        printf("[Bookmark] Error: Maximum number of bookmarks reached\n");
    }
}

// Remove a bookmark
void remove_bookmark(SharedState *state, int bookmark_index, int sender_tab_id) {
    if (!state) return;
    
    lock_shared_memory();
    
    if (bookmark_index >= 0 && bookmark_index < state->bookmark_count) {
        char message[BROADCAST_MSG_SIZE];
        snprintf(message, BROADCAST_MSG_SIZE, "%s (%s)", 
                 state->bookmarks[bookmark_index].title, 
                 state->bookmarks[bookmark_index].url);
        
        // Mark as inactive
        state->bookmarks[bookmark_index].is_active = false;
        
        // Shift bookmarks (or just mark as inactive to avoid shifting)
        /*
        for (int i = bookmark_index; i < state->bookmark_count - 1; i++) {
            state->bookmarks[i] = state->bookmarks[i + 1];
        }
        state->bookmark_count--;
        */
        
        unlock_shared_memory();
        
        broadcast_message(state, BROADCAST_BOOKMARK_REMOVED, sender_tab_id, message);
    } else {
        unlock_shared_memory();
        printf("[Bookmark] Error: Invalid bookmark index\n");
    }
}

// Clean up shared resources
void cleanup_shared_resources(int shmid, int semid) {
    // Clean up shared memory
    if (shmid >= 0) {
        shmctl(shmid, IPC_RMID, NULL);
    }
    
    // Clean up semaphore
    if (semid >= 0) {
        semctl(semid, 0, IPC_RMID);
    }
} 