#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <pthread.h>
#include <ncurses.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <signal.h>
#include <time.h>
#include <menu.h>
#include <panel.h>
#include "common.h"
#include "shared_memory.h"

// UI Constants
#define COLOR_TITLE     1
#define COLOR_STATUS    2
#define COLOR_NOTIFY    3
#define COLOR_URL       4
#define COLOR_CONTENT   5
#define COLOR_MENU      6
#define COLOR_HIGHLIGHT 7
#define COLOR_WARNING   8

int tab_id;
int write_fd;
char response_fifo[64];
WINDOW *mainwin;
WINDOW *cmdwin;
WINDOW *contentwin;
WINDOW *statuswin;
WINDOW *notificationwin;
WINDOW *titlewin;
WINDOW *menuwin;
PANEL *panels[5];
int shmid = -1;
int semid = -1;
SharedState *shared_state = NULL;
pthread_t response_thread;
pthread_t sync_thread;
int running = 1;
int is_synced = 0;
char notification[MAX_MSG];
time_t notification_time = 0;
int show_menu = 0;
int selected_menu_item = 0;

// Menu items
char *menu_items[] = {
    "Load Page",
    "Reload",
    "Back",
    "Forward",
    "Bookmarks",
    "History",
    "Toggle Sync",
    "Exit"
};
int num_menu_items = 8;

// Tab state
char current_url[MAX_MSG] = "";
int is_connected = 0;

void cleanup() {
    // Stop threads
    running = 0;
    pthread_cancel(response_thread);
    pthread_cancel(sync_thread);
    
    // Clean up shared memory if attached
    if (shared_state != NULL) {
        // Update tab status in shared memory
        if (is_synced) {
            lock_shared_memory();
            shared_state->tab_active[tab_id % MAX_TABS] = false;
            unlock_shared_memory();
            
            // Broadcast tab closed message
            broadcast_message(shared_state, BROADCAST_TAB_CLOSED, tab_id, "Tab closed");
        }
        
        detach_shared_memory(shared_state);
        printf("[Tab %d] Detached from shared memory.\n", tab_id);
    }
    
    // Remove FIFO
    unlink(response_fifo);
    printf("[Tab %d] FIFO removed.\n", tab_id);
}

void signal_handler(int sig) {
    printf("[Tab %d] Caught signal %d, cleaning up...\n", tab_id, sig);
    cleanup();
    endwin();  // End ncurses
    exit(0);
}

// Attach to shared memory
int attach_shared_memory() {
    // Try to access shared memory
    shmid = shmget(SHM_KEY, sizeof(SharedState), 0666);
    if (shmid < 0) {
        // Can't access shared memory yet
        return -1;
    }
    
    // Attach to shared memory
    shared_state = attach_shared_memory(shmid);
    if (!shared_state) {
        perror("shmat");
        return -1;
    }
    
    // Try to access semaphores
    semid = semget(SEM_KEY, 1, 0666);
    if (semid < 0) {
        perror("semget");
        detach_shared_memory(shared_state);
        shared_state = NULL;
        return -1;
    }
    
    printf("[Tab %d] Attached to shared memory with ID: %d\n", tab_id, shmid);
    return 0;
}

// Draw decorative borders
void draw_borders(WINDOW *win) {
    int x, y, i;
    getmaxyx(win, y, x);
    
    // Draw corners
    mvwaddch(win, 0, 0, ACS_ULCORNER);
// Show notification message
void show_notification(const char *message) {
    if (!notificationwin) return;
    
    strncpy(notification, message, MAX_MSG - 1);
    notification_time = time(NULL);
    
    // Clear notification window
    werase(notificationwin);
    
    wattron(notificationwin, A_REVERSE);
    mvwprintw(notificationwin, 0, 1, "NOTIFICATION: %s", notification);
    wattroff(notificationwin, A_REVERSE);
    
    wrefresh(notificationwin);
}

// Process broadcast messages
void *sync_thread_func(void *arg) {
    // Try to attach to shared memory if not already attached
    if (!shared_state && attach_shared_memory() < 0) {
        printf("[Tab %d] Failed to attach to shared memory for sync thread\n", tab_id);
        pthread_exit(NULL);
    }
    
    while (running) {
        if (shared_state && is_synced) {
            // Check for new broadcasts
            if (check_new_broadcasts(shared_state, tab_id)) {
                lock_shared_memory();
                
                // Process each message
                for (int i = 0; i < 10; i++) {
                    BroadcastMessage *msg = &shared_state->broadcast_messages[i];
                    
                    if (msg->timestamp > 0 && !msg->processed[tab_id]) {
                        // Mark as processed
                        msg->processed[tab_id] = true;
                        
                        // Only process if not from self
                        if (msg->sender_tab_id != tab_id) {
                            char notification_msg[MAX_MSG];
                            
                            // Process based on type
                            switch (msg->type) {
                                case BROADCAST_BOOKMARK_ADDED:
                                    snprintf(notification_msg, MAX_MSG, 
                                            "Tab %d added bookmark: %s", 
                                            msg->sender_tab_id, msg->data);
                                    break;
                                    
                                case BROADCAST_BOOKMARK_REMOVED:
                                    snprintf(notification_msg, MAX_MSG, 
                                            "Tab %d removed bookmark: %s", 
                                            msg->sender_tab_id, msg->data);
                                    break;
                                    
                                case BROADCAST_NEW_TAB:
                                    snprintf(notification_msg, MAX_MSG, 
                                            "New tab opened: %d", 
                                            msg->sender_tab_id);
                                    break;
                                    
                                case BROADCAST_TAB_CLOSED:
                                    snprintf(notification_msg, MAX_MSG, 
                                            "Tab %d closed", 
                                            msg->sender_tab_id);
                                    break;
                                    
                                case BROADCAST_PAGE_LOADED:
                                    snprintf(notification_msg, MAX_MSG, 
                                            "Tab %d loaded page: %s", 
                                            msg->sender_tab_id, msg->data);
                                    break;
                                    
                                default:
                                    continue; // Skip showing notification
                            }
                            
                            // Show notification
                            show_notification(notification_msg);
                        }
                    }
                }
                
                unlock_shared_memory();
            }
        }
        
        // Check every second
        sleep(1);
    }
    
    return NULL;
}

void *listen_response(void *arg) {
    int read_fd = open(response_fifo, O_RDONLY);
    if (read_fd < 0) {
        perror("open response fifo");
        pthread_exit(NULL);
    }

    char response[MAX_MSG * 2];
    while (running) {
        // Use non-blocking reads with a timeout
        fd_set readfds;
        struct timeval tv;
        
        FD_ZERO(&readfds);
        FD_SET(read_fd, &readfds);
        
        // Set 1 second timeout
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        
        int result = select(read_fd + 1, &readfds, NULL, NULL, &tv);
        
        if (result > 0 && FD_ISSET(read_fd, &readfds)) {
            // Clear buffer
            memset(response, 0, sizeof(response));
            
            // Use buffered read for better performance
            size_t total_read = 0;
            size_t bytes_read = 0;
            
            do {
                bytes_read = read(read_fd, response + total_read, 
                                 sizeof(response) - total_read - 1);
                if (bytes_read > 0) {
                    total_read += bytes_read;
                }
            } while (bytes_read > 0 && total_read < sizeof(response) - 1);
            
            if (total_read > 0) {
                // Ensure null termination
                response[total_read] = '\0';
                
                // Process response
                werase(contentwin);
                
                int line = 1;
                char *token = strtok(response, "\n");
                while (token && line < LINES - 8) {
                    mvwprintw(contentwin, line++, 1, "%s", token);
                    token = strtok(NULL, "\n");
                }
                
                box(contentwin, 0, 0);
                
                // Update address bar with current URL if available
                if (current_url[0] != '\0') {
                    werase(cmdwin);
                    box(cmdwin, 0, 0);
                    mvwprintw(cmdwin, 1, 2, "URL: %s", current_url);
                    mvwprintw(cmdwin, 3, 2, "Command > ");
                    wrefresh(cmdwin);
                }
                
                wrefresh(contentwin);
            }
        }
    }

    close(read_fd);
    return NULL;
}

void update_ui() {
    clear();
    
    // Create command window
    cmdwin = newwin(5, COLS - 2, 1, 1);
    box(cmdwin, 0, 0);
    mvwprintw(cmdwin, 1, 2, "URL: %s", current_url);
    mvwprintw(cmdwin, 3, 2, "Command > ");
    
    // Create content window
    contentwin = newwin(LINES - 9, COLS - 2, 6, 1);
    box(contentwin, 0, 0);
    mvwprintw(contentwin, 1, 2, "Content will appear here...");
    
    // Create notification window
    notificationwin = newwin(1, COLS - 2, LINES - 3, 1);
    
    // Create status window
    statuswin = newwin(1, COLS - 2, LINES - 2, 1);
    wattron(statuswin, A_REVERSE);
    mvwprintw(statuswin, 0, 1, "Status: %s | Sync: %s", 
             is_connected ? "Connected" : "Disconnected",
             is_synced ? "ON" : "OFF");
    wattroff(statuswin, A_REVERSE);
    
    // Draw status bar
    attron(A_REVERSE);
    mvprintw(0, 0, "Mini Browser - Tab %d%*s", 
             tab_id, COLS - 16 - (tab_id > 9 ? 2 : 1), "");
    attroff(A_REVERSE);
    
    // Instructions at bottom
    attron(A_REVERSE);
    mvprintw(LINES - 1, 0, 
          "Commands: load <page>, reload, back, forward, history, bookmark, bookmarks, sync on/off%*s", 
          COLS - 80, "");
    attroff(A_REVERSE);
    
    refresh();
    wrefresh(cmdwin);
    wrefresh(contentwin);
    wrefresh(statuswin);
}

void update_status() {
    if (!statuswin) return;
    
    werase(statuswin);
    wattron(statuswin, A_REVERSE);
    mvwprintw(statuswin, 0, 1, "Status: %s | Sync: %s | Tab ID: %d", 
             is_connected ? "Connected" : "Disconnected",
             is_synced ? "ON" : "OFF",
             tab_id);
    
    // Add shared memory status if available
    if (shared_state) {
        wprintw(statuswin, " | Active Tabs: %d", shared_state->active_tab_count);
    }
    
    // Fill the rest with spaces
    int x, y;
    getyx(statuswin, y, x);
    for (int i = x; i < COLS - 3; i++) {
        waddch(statuswin, ' ');
    }
    
    wattroff(statuswin, A_REVERSE);
    wrefresh(statuswin);
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Usage: %s <tab_id>\n", argv[0]);
        return 1;
    }

    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    tab_id = atoi(argv[1]);
    snprintf(response_fifo, sizeof(response_fifo), "%s%d", RESPONSE_FIFO_PREFIX, tab_id);
    mkfifo(response_fifo, 0666);

    // Try to connect to browser via FIFO
    write_fd = open(BROWSER_FIFO, O_WRONLY);
    if (write_fd < 0) {
        perror("open browser fifo");
        printf("[Tab %d] Cannot connect to browser. Make sure it's running.\n", tab_id);
        exit(1);
    }
    
    is_connected = 1;
    
    // Try to attach to shared memory
    if (attach_shared_memory() == 0) {
        printf("[Tab %d] Shared memory attached successfully.\n", tab_id);
    } else {
        printf("[Tab %d] No shared memory yet. Will try again later.\n", tab_id);
    }

    BrowserMessage msg;
    msg.tab_id = tab_id;
    msg.cmd_type = CMD_UNKNOWN;
    msg.use_shared_memory = 0;
    msg.shared_memory_id = -1;
    msg.timestamp = time(NULL);
    
    // Initialize ncurses
    initscr();
    cbreak();
    echo();
    keypad(stdscr, TRUE);
    curs_set(1);
    
    // Check if terminal supports colors
    if (has_colors()) {
        start_color();
        init_pair(1, COLOR_WHITE, COLOR_BLUE);    // Status bar
        init_pair(2, COLOR_BLACK, COLOR_GREEN);   // Notification
        init_pair(3, COLOR_BLACK, COLOR_YELLOW);  // Warning
    }
    
    update_ui();
    update_status();

    // Start response listener thread
    pthread_create(&response_thread, NULL, listen_response, NULL);
    
    // Start sync thread
    pthread_create(&sync_thread, NULL, sync_thread_func, NULL);
    
    char input[MAX_MSG];
    while (1) {
        // Update UI if needed
        if (notification_time > 0 && time(NULL) - notification_time > 5) {
            // Clear notification after 5 seconds
            werase(notificationwin);
            wrefresh(notificationwin);
            notification_time = 0;
        }
        
        // Position cursor for input
        wmove(cmdwin, 3, 12);  // Position cursor after "Command > "
        wgetnstr(cmdwin, input, MAX_MSG);
        
        if (strcmp(input, "exit") == 0) break;
        
        // Parse command
        if (strncmp(input, "load ", 5) == 0) {
            strncpy(current_url, input + 5, MAX_MSG - 1);
            current_url[MAX_MSG - 1] = '\0';
            update_ui();
            msg.cmd_type = CMD_LOAD;
        } else if (strcmp(input, "reload") == 0) {
            msg.cmd_type = CMD_RELOAD;
        } else if (strcmp(input, "back") == 0) {
            msg.cmd_type = CMD_BACK;
        } else if (strcmp(input, "forward") == 0) {
            msg.cmd_type = CMD_FORWARD;
        } else if (strcmp(input, "history") == 0) {
            msg.cmd_type = CMD_HISTORY;
        } else if (strcmp(input, "bookmark") == 0) {
            msg.cmd_type = CMD_BOOKMARK;
        } else if (strcmp(input, "bookmarks") == 0) {
            msg.cmd_type = CMD_BOOKMARK_LIST;
        } else if (strncmp(input, "open ", 5) == 0) {
            msg.cmd_type = CMD_BOOKMARK_OPEN;
        } else if (strncmp(input, "delete ", 7) == 0) {
            msg.cmd_type = CMD_BOOKMARK_DELETE;
        } else if (strcmp(input, "sync on") == 0) {
            msg.cmd_type = CMD_SYNC_ON;
            is_synced = 1;
            update_status();
        } else if (strcmp(input, "sync off") == 0) {
            msg.cmd_type = CMD_SYNC_OFF;
            is_synced = 0;
            update_status();
        } else if (strncmp(input, "broadcast ", 10) == 0) {
            msg.cmd_type = CMD_BROADCAST;
        } else if (strcmp(input, "status") == 0) {
            msg.cmd_type = CMD_STATUS;
        } else {
            msg.cmd_type = CMD_UNKNOWN;
        }
        
        // Set timestamp
        msg.timestamp = time(NULL);
        
        // Send command to browser
        strncpy(msg.command, input, MAX_MSG);
        write(write_fd, &msg, sizeof(msg));

        // Update command window
        werase(cmdwin);
        box(cmdwin, 0, 0);
        mvwprintw(cmdwin, 1, 2, "URL: %s", current_url);
        mvwprintw(cmdwin, 3, 2, "Command > ");
        wrefresh(cmdwin);
    }

    endwin();
    cleanup();
    close(write_fd);
    return 0;
}

