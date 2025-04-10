#ifndef SHARED_MEMORY_H
#define SHARED_MEMORY_H

#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <stdbool.h>
#include "common.h"

#define SHM_KEY 9876
#define SEM_KEY 5432
#define MAX_BOOKMARKS 50
#define MAX_URL_LENGTH 256
#define BROADCAST_MSG_SIZE 1024

// Semaphore operations
union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
};

// Bookmark structure
typedef struct {
    char url[MAX_URL_LENGTH];
    char title[MAX_URL_LENGTH];
    bool is_active;
} Bookmark;

// Broadcast message types
typedef enum {
    BROADCAST_BOOKMARK_ADDED,
    BROADCAST_BOOKMARK_REMOVED,
    BROADCAST_NEW_TAB,
    BROADCAST_TAB_CLOSED,
    BROADCAST_PAGE_LOADED
} BroadcastType;

// Broadcast message structure
typedef struct {
    BroadcastType type;
    int sender_tab_id;
    time_t timestamp;
    char data[BROADCAST_MSG_SIZE];
    bool processed[MAX_TABS];
} BroadcastMessage;

// Shared memory structure for synchronization between tabs
typedef struct {
    // Active tabs tracking
    bool tab_active[MAX_TABS];
    int active_tab_count;
    
    // Shared bookmarks
    Bookmark bookmarks[MAX_BOOKMARKS];
    int bookmark_count;
    
    // Broadcast messaging system
    BroadcastMessage broadcast_messages[10];
    int broadcast_count;
    
    // Global statistics
    int total_pages_loaded;
    char last_loaded_url[MAX_URL_LENGTH];
    time_t last_activity;
} SharedState;

// Function prototypes
int init_shared_memory();
int init_semaphores();
void lock_shared_memory();
void unlock_shared_memory();
void *attach_shared_memory(int shmid);
void detach_shared_memory(void *segment);
void broadcast_message(SharedState *state, BroadcastType type, int sender_tab_id, const char *data);
bool check_new_broadcasts(SharedState *state, int tab_id);
void process_broadcasts(SharedState *state, int tab_id);
void add_bookmark(SharedState *state, const char *url, const char *title, int sender_tab_id);
void remove_bookmark(SharedState *state, int bookmark_index, int sender_tab_id);
void cleanup_shared_resources(int shmid, int semid);

#endif 