#ifndef COMMON_H
#define COMMON_H

#include <time.h>

#define MAX_MSG 512
#define BROWSER_FIFO "/tmp/browser_fifo"
#define RESPONSE_FIFO_PREFIX "/tmp/tab_response_"
#define SHM_KEY 9876
#define MAX_TABS 10

// Command types
typedef enum {
    CMD_LOAD,           // Load a page
    CMD_RELOAD,         // Reload current page
    CMD_BACK,           // Go back in history
    CMD_FORWARD,        // Go forward in history
    CMD_BOOKMARK,       // Bookmark current page
    CMD_BOOKMARK_LIST,  // List all bookmarks
    CMD_BOOKMARK_OPEN,  // Open a bookmark
    CMD_BOOKMARK_DELETE,// Delete a bookmark
    CMD_HISTORY,        // Show history
    CMD_SYNC_ON,        // Enable tab synchronization
    CMD_SYNC_OFF,       // Disable tab synchronization
    CMD_BROADCAST,      // Send message to all tabs
    CMD_STATUS,         // Show browser status
    CMD_CRASH,          // Simulate crash
    CMD_UNKNOWN         // Unknown command
} CommandType;

typedef struct {
    int tab_id;
    CommandType cmd_type;
    char command[MAX_MSG];
    int use_shared_memory;       // Flag to indicate if shared memory is used
    int shared_memory_id;        // ID of shared memory segment if used
    time_t timestamp;            // Timestamp of the command
} BrowserMessage;

// Tab state
typedef struct {
    int tab_id;
    char current_url[MAX_MSG];
    char history[10][MAX_MSG];   // Simple history - last 10 pages
    int history_count;
    int history_position;
    time_t last_active;          // Last time this tab was active
    int is_synced;               // Whether this tab is synced with others
} TabState;

#endif

