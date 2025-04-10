#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include "common.h"
#include "shared_memory.h"

// Global state
TabState tab_states[MAX_TABS];
int shmid = -1;
int semid = -1;
SharedState *shared_state = NULL;
pthread_t broadcast_thread;
int running = 1;

// Buffer for large data
char content_buffer[MAX_MSG * 10];

void cleanup() {
    // Stop broadcast thread
    running = 0;
    pthread_join(broadcast_thread, NULL);
    
    // Clean up shared memory and semaphores
    if (shared_state != NULL) {
        detach_shared_memory(shared_state);
    }
    
    cleanup_shared_resources(shmid, semid);
    
    // Remove FIFO
    unlink(BROWSER_FIFO);
    printf("[Browser] Resources cleaned up.\n");
}

void signal_handler(int sig) {
    printf("[Browser] Caught signal %d, cleaning up...\n", sig);
    cleanup();
    exit(0);
}

// Thread to check for inactive tabs and manage broadcast notifications
void *broadcast_manager(void *arg) {
    printf("[Browser] Broadcast manager thread started\n");
    
    while (running) {
        if (shared_state) {
            lock_shared_memory();
            
            // Update active tabs
            time_t now = time(NULL);
            for (int i = 0; i < MAX_TABS; i++) {
                if (tab_states[i].tab_id > 0) {
                    // Check if tab is still active (within last 30 seconds)
                    if (now - tab_states[i].last_active > 30) {
                        printf("[Browser] Tab %d appears to be inactive\n", tab_states[i].tab_id);
                        shared_state->tab_active[i] = false;
                    }
                }
            }
            
            // Update global statistics
            shared_state->last_activity = now;
            
            unlock_shared_memory();
        }
        
        // Check every 5 seconds
        sleep(5);
    }
    
    return NULL;
}

void send_response(int tab_id, const char *response) {
    char path[64];
    snprintf(path, sizeof(path), "%s%d", RESPONSE_FIFO_PREFIX, tab_id);
    int fd = open(path, O_WRONLY | O_NONBLOCK);
    if (fd >= 0) {
        // Attempt to optimize large writes
        size_t len = strlen(response) + 1;
        size_t written = 0;
        
        // Batch write for better performance
        while (written < len) {
            ssize_t bytes = write(fd, response + written, len - written);
            if (bytes <= 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // FIFO buffer full, sleep a bit and retry
                    usleep(1000);
                    continue;
                }
                break; // Other error
            }
            written += bytes;
        }
        
        close(fd);
    } else {
        perror("open response fifo");
    }
}

void render_html_with_w3m(const char *html_file, char *output) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "w3m -dump %s > /tmp/rendered.txt", html_file);
    int ret = system(cmd);
    if (ret == -1) {
        strcpy(output, "[Browser] Error: Failed to execute w3m command.");
        return;
    } else if (WEXITSTATUS(ret) != 0) {
        sprintf(output, "[Browser] Error: w3m command failed with status %d", WEXITSTATUS(ret));
        return;
    }

    FILE *fp = fopen("/tmp/rendered.txt", "r");
    if (!fp) {
        strcpy(output, "[Browser] Error: Cannot open rendered output.");
        return;
    }

    char line[256];
    output[0] = '\0';
    while (fgets(line, sizeof(line), fp)) {
        if (strlen(output) + strlen(line) < MAX_MSG - 1)
            strcat(output, line);
    }
    fclose(fp);
}

// Log history for a tab
void log_history(int tab_id, const char *url) {
    TabState *state = &tab_states[tab_id % MAX_TABS];
    
    // If we're not at the end of history, truncate it
    if (state->history_position < state->history_count - 1) {
        state->history_count = state->history_position + 1;
    }
    
    // Add new entry
    if (state->history_count < 10) {
        strcpy(state->history[state->history_count], url);
        state->history_count++;
        state->history_position = state->history_count - 1;
    } else {
        // Shift history
        for (int i = 0; i < 9; i++) {
            strcpy(state->history[i], state->history[i+1]);
        }
        strcpy(state->history[9], url);
        state->history_position = 9;
    }
    
    // Update current URL
    strcpy(state->current_url, url);
    
    // Update last active time
    state->last_active = time(NULL);
    
    // Update shared state if synchronized
    if (state->is_synced && shared_state) {
        lock_shared_memory();
        
        // Update global statistics
        shared_state->total_pages_loaded++;
        strncpy(shared_state->last_loaded_url, url, MAX_URL_LENGTH - 1);
        
        unlock_shared_memory();
        
        // Broadcast to other tabs
        if (shared_state) {
            broadcast_message(shared_state, BROADCAST_PAGE_LOADED, tab_id, url);
        }
    }
}

// Get command type from text command
CommandType get_command_type(const char* cmd) {
    if (strncmp(cmd, "load ", 5) == 0) return CMD_LOAD;
    if (strcmp(cmd, "reload") == 0) return CMD_RELOAD;
    if (strcmp(cmd, "back") == 0) return CMD_BACK;
    if (strcmp(cmd, "forward") == 0) return CMD_FORWARD;
    if (strcmp(cmd, "history") == 0) return CMD_HISTORY;
    if (strcmp(cmd, "bookmark") == 0) return CMD_BOOKMARK;
    if (strcmp(cmd, "bookmarks") == 0) return CMD_BOOKMARK_LIST;
    if (strncmp(cmd, "open ", 5) == 0) return CMD_BOOKMARK_OPEN;
    if (strncmp(cmd, "delete ", 7) == 0) return CMD_BOOKMARK_DELETE;
    if (strcmp(cmd, "sync on") == 0) return CMD_SYNC_ON;
    if (strcmp(cmd, "sync off") == 0) return CMD_SYNC_OFF;
    if (strncmp(cmd, "broadcast ", 10) == 0) return CMD_BROADCAST;
    if (strcmp(cmd, "status") == 0) return CMD_STATUS;
    if (strcmp(cmd, "CRASH") == 0) return CMD_CRASH;
    return CMD_UNKNOWN;
}

// Show all bookmarks in shared memory
void list_bookmarks(int tab_id) {
    if (!shared_state) {
        send_response(tab_id, "[Browser] Bookmarks not available (shared memory not initialized)");
        return;
    }
    
    lock_shared_memory();
    
    if (shared_state->bookmark_count == 0) {
        unlock_shared_memory();
        send_response(tab_id, "[Browser] No bookmarks available.");
        return;
    }
    
    char buffer[MAX_MSG * 5] = "[Browser] Bookmarks:\n";
    
    for (int i = 0; i < shared_state->bookmark_count; i++) {
        if (shared_state->bookmarks[i].is_active) {
            char entry[MAX_MSG];
            snprintf(entry, sizeof(entry), "%d: %s (%s)\n", 
                    i + 1, 
                    shared_state->bookmarks[i].title,
                    shared_state->bookmarks[i].url);
            
            if (strlen(buffer) + strlen(entry) < sizeof(buffer) - 1) {
                strcat(buffer, entry);
            }
        }
    }
    
    unlock_shared_memory();
    send_response(tab_id, buffer);
}

// Show browser status
void show_browser_status(int tab_id) {
    if (!shared_state) {
        send_response(tab_id, "[Browser] Status not available (shared memory not initialized)");
        return;
    }
    
    lock_shared_memory();
    
    char buffer[MAX_MSG * 2] = "[Browser] Status:\n";
    char entry[512];
    
    // Active tabs
    int active_count = 0;
    for (int i = 0; i < MAX_TABS; i++) {
        if (shared_state->tab_active[i]) {
            active_count++;
        }
    }
    
    snprintf(entry, sizeof(entry), "Active tabs: %d\n", active_count);
    strcat(buffer, entry);
    
    // Pages loaded
    snprintf(entry, sizeof(entry), "Total pages loaded: %d\n", shared_state->total_pages_loaded);
    strcat(buffer, entry);
    
    // Last activity
    struct tm *timeinfo = localtime(&shared_state->last_activity);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%H:%M:%S", timeinfo);
    
    snprintf(entry, sizeof(entry), "Last activity: %s\n", time_str);
    strcat(buffer, entry);
    
    // Last loaded URL
    if (shared_state->last_loaded_url[0] != '\0') {
        snprintf(entry, sizeof(entry), "Last loaded URL: %.*s\n", 
                (int)(sizeof(entry) - 20), shared_state->last_loaded_url);
        strcat(buffer, entry);
    }
    
    // Bookmarks
    snprintf(entry, sizeof(entry), "Bookmarks: %d\n", shared_state->bookmark_count);
    strcat(buffer, entry);
    
    unlock_shared_memory();
    send_response(tab_id, buffer);
}

void handle_command(BrowserMessage *msg) {
    TabState *state = &tab_states[msg->tab_id % MAX_TABS];
    
    // Initialize tab state if first command
    if (state->tab_id == 0) {
        state->tab_id = msg->tab_id;
        state->history_count = 0;
        state->history_position = -1;
        state->current_url[0] = '\0';
        state->last_active = time(NULL);
        state->is_synced = 0;
        
        // Update shared memory tab activity
        if (shared_state) {
            lock_shared_memory();
            shared_state->tab_active[msg->tab_id % MAX_TABS] = true;
            shared_state->active_tab_count++;
            unlock_shared_memory();
            
            // Broadcast new tab
            broadcast_message(shared_state, BROADCAST_NEW_TAB, msg->tab_id, "New tab opened");
        }
    }
    
    // Update last activity time
    state->last_active = time(NULL);
    
    // Set command type if not set
    if (msg->cmd_type == CMD_UNKNOWN) {
        msg->cmd_type = get_command_type(msg->command);
    }
    
    char response[MAX_MSG];
    
    switch (msg->cmd_type) {
        case CMD_LOAD: {
            char path[512];
            char page_name[256];
            char html_file[512];

            strcpy(path, msg->command + 5);
            strcpy(page_name, basename(path));
            
            // Remove any file extension
            char *dot = strrchr(page_name, '.');
            if (dot) *dot = '\0';
            
            // Đảm bảo page_name đủ ngắn để không gây tràn bộ đệm
            if (strlen(page_name) > 500) {
                page_name[500] = '\0';
            }
            
            // Tạo tên file HTML
            snprintf(html_file, sizeof(html_file), "%s.html", page_name);

            FILE *check = fopen(html_file, "r");
            if (!check) {
                send_response(msg->tab_id, "[Browser] Error: Page not found.");
                return;
            }
            fclose(check);

            // Log to history
            log_history(msg->tab_id, page_name);

            char content[MAX_MSG];
            render_html_with_w3m(html_file, content);
            send_response(msg->tab_id, content);
            break;
        }
        
        case CMD_RELOAD:
            if (state->current_url[0] == '\0') {
                send_response(msg->tab_id, "[Browser] No page to reload.");
            } else {
                char html_file[MAX_MSG];
                snprintf(html_file, sizeof(html_file), "%s.html", state->current_url);
                
                char content[MAX_MSG];
                render_html_with_w3m(html_file, content);
                send_response(msg->tab_id, content);
                
                printf("[Browser] Tab %d reloaded: %s\n", msg->tab_id, state->current_url);
            }
            break;
            
        case CMD_BACK:
            if (state->history_position > 0) {
                state->history_position--;
                strcpy(state->current_url, state->history[state->history_position]);
                
                char html_file[MAX_MSG];
                snprintf(html_file, sizeof(html_file), "%s.html", state->current_url);
                
                char content[MAX_MSG];
                render_html_with_w3m(html_file, content);
                send_response(msg->tab_id, content);
                
                printf("[Browser] Tab %d navigated back to: %s\n", 
                       msg->tab_id, state->current_url);
            } else {
                send_response(msg->tab_id, "[Browser] No previous page in history.");
            }
            break;
            
        case CMD_FORWARD:
            if (state->history_position < state->history_count - 1) {
                state->history_position++;
                strcpy(state->current_url, state->history[state->history_position]);
                
                char html_file[MAX_MSG];
                snprintf(html_file, sizeof(html_file), "%s.html", state->current_url);
                
                char content[MAX_MSG];
                render_html_with_w3m(html_file, content);
                send_response(msg->tab_id, content);
                
                printf("[Browser] Tab %d navigated forward to: %s\n", 
                       msg->tab_id, state->current_url);
            } else {
                send_response(msg->tab_id, "[Browser] No next page in history.");
            }
            break;
            
        case CMD_BOOKMARK:
            if (state->current_url[0] == '\0') {
                send_response(msg->tab_id, "[Browser] No page to bookmark.");
            } else if (!shared_state) {
                send_response(msg->tab_id, "[Browser] Bookmark feature requires shared memory.");
            } else {
                add_bookmark(shared_state, state->current_url, state->current_url, msg->tab_id);
                snprintf(response, sizeof(response), 
                        "[Browser] Bookmarked: %s", state->current_url);
                send_response(msg->tab_id, response);
            }
            break;
            
        case CMD_BOOKMARK_LIST:
            list_bookmarks(msg->tab_id);
            break;
            
        case CMD_BOOKMARK_OPEN: {
            if (!shared_state) {
                send_response(msg->tab_id, "[Browser] Bookmark feature requires shared memory.");
                break;
            }
            
            int index;
            if (sscanf(msg->command + 5, "%d", &index) != 1 || index < 1) {
                send_response(msg->tab_id, "[Browser] Invalid bookmark number. Use 'open <number>'");
                break;
            }
            
            lock_shared_memory();
            
            if (index > shared_state->bookmark_count || 
                !shared_state->bookmarks[index-1].is_active) {
                unlock_shared_memory();
                send_response(msg->tab_id, "[Browser] Invalid bookmark number.");
                break;
            }
            
            char url[MAX_URL_LENGTH];
            strncpy(url, shared_state->bookmarks[index-1].url, MAX_URL_LENGTH);
            
            unlock_shared_memory();
            
            // Load the bookmarked page
            char html_file[MAX_MSG];
            snprintf(html_file, sizeof(html_file), "%s.html", url);
            
            FILE *check = fopen(html_file, "r");
            if (!check) {
                send_response(msg->tab_id, "[Browser] Error: Bookmarked page not found.");
                break;
            }
            fclose(check);
            
            // Log to history
            log_history(msg->tab_id, url);
            
            char content[MAX_MSG];
            render_html_with_w3m(html_file, content);
            send_response(msg->tab_id, content);
            break;
        }
            
        case CMD_BOOKMARK_DELETE: {
            if (!shared_state) {
                send_response(msg->tab_id, "[Browser] Bookmark feature requires shared memory.");
                break;
            }
            
            int index;
            if (sscanf(msg->command + 7, "%d", &index) != 1 || index < 1) {
                send_response(msg->tab_id, "[Browser] Invalid bookmark number. Use 'delete <number>'");
                break;
            }
            
            remove_bookmark(shared_state, index-1, msg->tab_id);
            
            snprintf(response, sizeof(response), 
                    "[Browser] Deleted bookmark #%d", index);
            send_response(msg->tab_id, response);
            break;
        }
            
        case CMD_HISTORY: {
            char history_text[MAX_MSG] = "[Browser] History:\n";
            
            if (state->history_count == 0) {
                strcat(history_text, "  (Empty)\n");
            } else {
                for (int i = 0; i < state->history_count; i++) {
                    char entry[MAX_MSG];
                    snprintf(entry, sizeof(entry), "  %d: %s %s\n", 
                             i + 1, 
                             (i == state->history_position) ? ">" : " ", 
                             state->history[i]);
                    
                    if (strlen(history_text) + strlen(entry) < MAX_MSG - 1) {
                        strcat(history_text, entry);
                    } else {
                        break;
                    }
                }
            }
            
            send_response(msg->tab_id, history_text);
            break;
        }
        
        case CMD_SYNC_ON:
            if (!shared_state) {
                send_response(msg->tab_id, "[Browser] Synchronization requires shared memory.");
            } else {
                state->is_synced = 1;
                
                lock_shared_memory();
                shared_state->tab_active[msg->tab_id % MAX_TABS] = true;
                unlock_shared_memory();
                
                send_response(msg->tab_id, "[Browser] Tab synchronization enabled.");
                
                // Process any pending broadcasts
                if (shared_state) {
                    process_broadcasts(shared_state, msg->tab_id);
                }
            }
            break;
            
        case CMD_SYNC_OFF:
            state->is_synced = 0;
            
            if (shared_state) {
                lock_shared_memory();
                shared_state->tab_active[msg->tab_id % MAX_TABS] = false;
                unlock_shared_memory();
            }
            
            send_response(msg->tab_id, "[Browser] Tab synchronization disabled.");
            break;
            
        case CMD_BROADCAST:
            if (!shared_state) {
                send_response(msg->tab_id, "[Browser] Broadcasting requires shared memory.");
            } else if (!state->is_synced) {
                send_response(msg->tab_id, "[Browser] Tab must be synced to broadcast messages.");
            } else {
                char message[BROADCAST_MSG_SIZE];
                strncpy(message, msg->command + 10, BROADCAST_MSG_SIZE - 1);
                
                broadcast_message(shared_state, BROADCAST_NEW_TAB, msg->tab_id, message);
                
                send_response(msg->tab_id, "[Browser] Message broadcasted to all synced tabs.");
            }
            break;
            
        case CMD_STATUS:
            show_browser_status(msg->tab_id);
            break;
            
        case CMD_CRASH:
            send_response(msg->tab_id, "[Browser] Tab crashed and recovered.");
            break;
            
        default:
            snprintf(response, sizeof(response), 
                    "[Browser] Unknown command: %s", msg->command);
            send_response(msg->tab_id, response);
            break;
    }
}

int main() {
    int fd;
    BrowserMessage msg;
    
    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Initialize tab states
    for (int i = 0; i < MAX_TABS; i++) {
        tab_states[i].tab_id = 0;
    }
    
    // Initialize shared memory
    shmid = init_shared_memory();
    if (shmid < 0) {
        fprintf(stderr, "Failed to initialize shared memory\n");
        return 1;
    }
    
    // Initialize semaphores for synchronization
    semid = init_semaphores();
    if (semid < 0) {
        fprintf(stderr, "Failed to initialize semaphores\n");
        return 1;
    }
    
    // Attach to shared memory
    shared_state = (SharedState *)attach_shared_memory(shmid);
    if (!shared_state) {
        fprintf(stderr, "Failed to attach shared memory\n");
        return 1;
    }
    
    // Create broadcast manager thread
    pthread_create(&broadcast_thread, NULL, broadcast_manager, NULL);
    
    // Create FIFO if it doesn't exist
    mkfifo(BROWSER_FIFO, 0666);

    printf("[Browser] Listening on %s...\n", BROWSER_FIFO);
    printf("[Browser] Shared memory active with key %d\n", SHM_KEY);
    printf("[Browser] Tab synchronization available\n");

    while (1) {
        fd = open(BROWSER_FIFO, O_RDONLY);
        if (fd < 0) {
            perror("open");
            continue;
        }

        while (read(fd, &msg, sizeof(msg)) > 0) {
            printf("[Browser] Tab %d sent: %s\n", msg.tab_id, msg.command);
            
            // Add timestamp
            msg.timestamp = time(NULL);
            
            handle_command(&msg);
        }

        close(fd);
    }

    cleanup();
    return 0;
}

