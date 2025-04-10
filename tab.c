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

// Menu items - Renamed to avoid conflict with menu.h
char *tab_menu_items[] = {
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

// Forward declaration
void update_status();

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

// Changed name to avoid conflict with shared_memory.h
int init_shared_memory_connection() {
    // Try to access shared memory
    shmid = shmget(SHM_KEY, sizeof(SharedState), 0666);
    if (shmid < 0) {
        // Can't access shared memory yet
        return -1;
    }
    
    // Attach to shared memory - use proper casting
    shared_state = (SharedState *)attach_shared_memory(shmid);
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
    mvwaddch(win, y - 1, 0, ACS_LLCORNER);
    mvwaddch(win, 0, x - 1, ACS_URCORNER);
    mvwaddch(win, y - 1, x - 1, ACS_LRCORNER);
    
    // Draw horizontal lines with fancy pattern
    for (i = 1; i < x - 1; i++) {
        mvwaddch(win, 0, i, ACS_HLINE);
        mvwaddch(win, y - 1, i, ACS_HLINE);
    }
    
    // Draw vertical lines with fancy pattern
    for (i = 1; i < y - 1; i++) {
        mvwaddch(win, i, 0, ACS_VLINE);
        mvwaddch(win, i, x - 1, ACS_VLINE);
    }
}

// Show notification message with animation
void show_notification(const char *message) {
    if (!notificationwin) return;
    
    strncpy(notification, message, MAX_MSG - 1);
    notification_time = time(NULL);
    
    // Clear notification window
    werase(notificationwin);
    
    // Set color
    wattron(notificationwin, COLOR_PAIR(COLOR_NOTIFY) | A_BOLD);
    
    // Draw notification with border
    draw_borders(notificationwin);
    mvwprintw(notificationwin, 1, 2, "▶ %s", notification);
    
    wattroff(notificationwin, COLOR_PAIR(COLOR_NOTIFY) | A_BOLD);
    wrefresh(notificationwin);
    
    // Flash notification to draw attention
    flash();
}

// Get active tab count
int get_active_tab_count() {
    if (!shared_state) return 0;
    
    lock_shared_memory();
    int count = 0;
    for (int i = 0; i < MAX_TABS; i++) {
        if (shared_state->tab_active[i]) {
            count++;
        }
    }
    unlock_shared_memory();
    
    return count;
}

// Show menu
void display_menu() {
    if (!menuwin) return;
    
    werase(menuwin);
    wattron(menuwin, COLOR_PAIR(COLOR_MENU));
    box(menuwin, 0, 0);
    
    mvwprintw(menuwin, 0, 2, " Menu ");
    
    for (int i = 0; i < num_menu_items; i++) {
        if (i == selected_menu_item) {
            wattron(menuwin, COLOR_PAIR(COLOR_HIGHLIGHT) | A_BOLD);
            mvwprintw(menuwin, i + 1, 2, "▶ %s", tab_menu_items[i]);
            wattroff(menuwin, COLOR_PAIR(COLOR_HIGHLIGHT) | A_BOLD);
        } else {
            mvwprintw(menuwin, i + 1, 2, "  %s", tab_menu_items[i]);
        }
    }
    
    wattroff(menuwin, COLOR_PAIR(COLOR_MENU));
    wrefresh(menuwin);
}

// Process broadcast messages
void *sync_thread_func(void *arg) {
    // Try to attach to shared memory if not already attached
    if (!shared_state && init_shared_memory_connection() < 0) {
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
                                            "💾 Tab %d added bookmark: %s", 
                                            msg->sender_tab_id, msg->data);
                                    break;
                                    
                                case BROADCAST_BOOKMARK_REMOVED:
                                    snprintf(notification_msg, MAX_MSG, 
                                            "🗑️ Tab %d removed bookmark: %s", 
                                            msg->sender_tab_id, msg->data);
                                    break;
                                    
                                case BROADCAST_NEW_TAB:
                                    snprintf(notification_msg, MAX_MSG, 
                                            "📄 New tab opened: %d", 
                                            msg->sender_tab_id);
                                    break;
                                    
                                case BROADCAST_TAB_CLOSED:
                                    snprintf(notification_msg, MAX_MSG, 
                                            "❌ Tab %d closed", 
                                            msg->sender_tab_id);
                                    break;
                                    
                                case BROADCAST_PAGE_LOADED:
                                    snprintf(notification_msg, MAX_MSG, 
                                            "🔄 Tab %d loaded page: %s", 
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
                
                // Draw styled border
                wattron(contentwin, COLOR_PAIR(COLOR_CONTENT));
                draw_borders(contentwin);
                mvwprintw(contentwin, 0, 2, " Content ");
                wattroff(contentwin, COLOR_PAIR(COLOR_CONTENT));
                
                // Display content
                wattron(contentwin, COLOR_PAIR(COLOR_CONTENT));
                
                // Get content window dimensions
                int win_rows, win_cols;
                getmaxyx(contentwin, win_rows, win_cols);
                
                int line = 1;
                char *token = strtok(response, "\n");
                while (token && line < win_rows - 1) {
                    // Ensure line doesn't exceed window width
                    if (strlen(token) > win_cols - 4) {
                        token[win_cols - 4] = '\0';
                    }
                    mvwprintw(contentwin, line++, 2, "%s", token);
                    token = strtok(NULL, "\n");
                }
                wattroff(contentwin, COLOR_PAIR(COLOR_CONTENT));
                
                // Update address bar with current URL if available
                if (current_url[0] != '\0') {
                    werase(cmdwin);
                    wattron(cmdwin, COLOR_PAIR(COLOR_URL));
                    draw_borders(cmdwin);
                    mvwprintw(cmdwin, 0, 2, " Location ");
                    
                    // URL field with icon - ensure it doesn't exceed window width
                    int cmd_cols;
                    getmaxyx(cmdwin, win_rows, cmd_cols);
                    if (strlen(current_url) > cmd_cols - 6) {
                        current_url[cmd_cols - 6] = '\0';
                    }
                    mvwprintw(cmdwin, 1, 2, "🔗 %s", current_url);
                    
                    // Command prompt with icon
                    mvwprintw(cmdwin, 3, 2, "💻 Command > ");
                    wattroff(cmdwin, COLOR_PAIR(COLOR_URL));
                    wrefresh(cmdwin);
                }
                
                wrefresh(contentwin);
                
                // Refresh status after content update
                update_status();
            }
        }
    }

    close(read_fd);
    return NULL;
}

void update_ui() {
    clear();
    
    // Get terminal dimensions
    int term_rows, term_cols;
    getmaxyx(stdscr, term_rows, term_cols);
    
    // Calculate window sizes based on terminal dimensions
    int title_height = 3;
    int cmd_height = 5;
    int notification_height = 3;
    int status_height = 2;
    int content_height = term_rows - (title_height + cmd_height + notification_height + status_height);
    
    // Ensure content window has minimum height
    if (content_height < 5) {
        content_height = 5;
    }
    
    // Create title bar
    titlewin = newwin(title_height, term_cols, 0, 0);
    wattron(titlewin, COLOR_PAIR(COLOR_TITLE) | A_BOLD);
    box(titlewin, 0, 0);
    
    // Center the title
    int title_pos = (term_cols - 32) / 2;
    if (title_pos < 2) title_pos = 2;
    
    mvwprintw(titlewin, 1, title_pos, "✨ Mini Browser - Tab %d ✨", tab_id);
    wattroff(titlewin, COLOR_PAIR(COLOR_TITLE) | A_BOLD);
    wrefresh(titlewin);
    
    // Create command window
    cmdwin = newwin(cmd_height, term_cols, title_height, 0);
    wattron(cmdwin, COLOR_PAIR(COLOR_URL));
    draw_borders(cmdwin);
    mvwprintw(cmdwin, 0, 2, " Location ");
    mvwprintw(cmdwin, 1, 2, "🔗 %s", current_url);
    mvwprintw(cmdwin, 3, 2, "💻 Command > ");
    wattroff(cmdwin, COLOR_PAIR(COLOR_URL));
    wrefresh(cmdwin);
    
    // Create content window
    int content_y = title_height + cmd_height;
    contentwin = newwin(content_height, term_cols, content_y, 0);
    wattron(contentwin, COLOR_PAIR(COLOR_CONTENT));
    draw_borders(contentwin);
    mvwprintw(contentwin, 0, 2, " Content ");
    mvwprintw(contentwin, 1, 2, "Content will appear here...");
    wattroff(contentwin, COLOR_PAIR(COLOR_CONTENT));
    wrefresh(contentwin);
    
    // Create notification window
    int notification_y = content_y + content_height;
    notificationwin = newwin(notification_height, term_cols, notification_y, 0);
    wattron(notificationwin, COLOR_PAIR(COLOR_NOTIFY));
    draw_borders(notificationwin);
    mvwprintw(notificationwin, 0, 2, " Notifications ");
    wattroff(notificationwin, COLOR_PAIR(COLOR_NOTIFY));
    wrefresh(notificationwin);
    
    // Create status window
    int status_y = notification_y + notification_height;
    statuswin = newwin(status_height, term_cols, status_y, 0);
    wattron(statuswin, COLOR_PAIR(COLOR_STATUS) | A_BOLD);
    mvwprintw(statuswin, 0, 1, " Status: %s | Sync: %s | Tab ID: %d | Press F1 for menu", 
             is_connected ? "Connected ✅" : "Disconnected ❌",
             is_synced ? "ON ✅" : "OFF ❌",
             tab_id);
    
    // Add keyboard shortcuts
    mvwprintw(statuswin, 1, 1, " F1:Menu | F2:Load | F3:Reload | F4:Back | F5:Forward | F10:Exit");
    wattroff(statuswin, COLOR_PAIR(COLOR_STATUS) | A_BOLD);
    wrefresh(statuswin);
    
    // Create menu window (hidden by default)
    menuwin = newwin(num_menu_items + 2, 20, 5, 5);
}

void update_status() {
    if (!statuswin) return;
    
    werase(statuswin);
    wattron(statuswin, COLOR_PAIR(COLOR_STATUS) | A_BOLD);
    
    // Active tabs count
    int active_tabs = get_active_tab_count();
    
    // Status line
    mvwprintw(statuswin, 0, 1, " Status: %s | Sync: %s | Tab ID: %d | Active Tabs: %d", 
             is_connected ? "Connected ✅" : "Disconnected ❌",
             is_synced ? "ON ✅" : "OFF ❌",
             tab_id,
             active_tabs);
    
    // Add bookmark count if available
    if (shared_state) {
        lock_shared_memory();
        wprintw(statuswin, " | Bookmarks: %d", shared_state->bookmark_count);
        unlock_shared_memory();
    }
    
    // Add keyboard shortcuts
    mvwprintw(statuswin, 1, 1, " F1:Menu | F2:Load | F3:Reload | F4:Back | F5:Forward | F10:Exit");
    
    wattroff(statuswin, COLOR_PAIR(COLOR_STATUS) | A_BOLD);
    wrefresh(statuswin);
}

// Handle menu selection
void handle_menu_action() {
    BrowserMessage msg;
    msg.tab_id = tab_id;
    msg.timestamp = time(NULL);
    msg.use_shared_memory = 0;
    msg.shared_memory_id = -1;
    char input[MAX_MSG];
    
    switch (selected_menu_item) {
        case 0: // Load Page
            show_notification("Enter URL to load");
            echo();
            wmove(cmdwin, 3, 15);
            wgetnstr(cmdwin, input, MAX_MSG - 6);
            noecho();
            
            if (strlen(input) > 0) {
                // Đảm bảo không tràn bộ đệm khi tạo command
                snprintf(msg.command, sizeof(msg.command), "load %.*s", 
                        (int)(sizeof(msg.command) - 6), input);
                msg.cmd_type = CMD_LOAD;
                strncpy(current_url, input, MAX_MSG - 1);
                current_url[MAX_MSG - 1] = '\0';
                update_ui();
                if (write(write_fd, &msg, sizeof(msg)) < 0) {
                    perror("write");
                }
            }
            break;
            
        case 1: // Reload
            strcpy(msg.command, "reload");
            msg.cmd_type = CMD_RELOAD;
            if (write(write_fd, &msg, sizeof(msg)) < 0) {
                perror("write");
            }
            break;
            
        case 2: // Back
            strcpy(msg.command, "back");
            msg.cmd_type = CMD_BACK;
            if (write(write_fd, &msg, sizeof(msg)) < 0) {
                perror("write");
            }
            break;
            
        case 3: // Forward
            strcpy(msg.command, "forward");
            msg.cmd_type = CMD_FORWARD;
            if (write(write_fd, &msg, sizeof(msg)) < 0) {
                perror("write");
            }
            break;
            
        case 4: // Bookmarks
            strcpy(msg.command, "bookmarks");
            msg.cmd_type = CMD_BOOKMARK_LIST;
            if (write(write_fd, &msg, sizeof(msg)) < 0) {
                perror("write");
            }
            break;
            
        case 5: // History
            strcpy(msg.command, "history");
            msg.cmd_type = CMD_HISTORY;
            if (write(write_fd, &msg, sizeof(msg)) < 0) {
                perror("write");
            }
            break;
            
        case 6: // Toggle Sync
            if (is_synced) {
                strcpy(msg.command, "sync off");
                msg.cmd_type = CMD_SYNC_OFF;
                is_synced = 0;
            } else {
                strcpy(msg.command, "sync on");
                msg.cmd_type = CMD_SYNC_ON;
                is_synced = 1;
            }
            update_status();
            if (write(write_fd, &msg, sizeof(msg)) < 0) {
                perror("write");
            }
            break;
            
        case 7: // Exit
            running = 0;
            break;
    }
    
    // Hide menu after action
    show_menu = 0;
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
    if (init_shared_memory_connection() == 0) {
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
    printf("[Tab %d] About to initialize ncurses...\n", tab_id);
    fflush(stdout);
    initscr();
    
    // Check terminal size
    int min_rows = 24;
    int min_cols = 80;
    int term_rows, term_cols;
    getmaxyx(stdscr, term_rows, term_cols);
    
    if (term_rows < min_rows || term_cols < min_cols) {
        endwin();
        printf("[Tab %d] Terminal too small. Minimum size required: %d x %d\n", 
               tab_id, min_cols, min_rows);
        printf("[Tab %d] Current terminal size: %d x %d\n", 
               tab_id, term_cols, term_rows);
        printf("Please resize your terminal and try again.\n");
        exit(1);
    }
    
    start_color();
    cbreak();
    keypad(stdscr, TRUE);
    noecho();
    curs_set(0);  // Hide cursor initially
    
    // Define color pairs
    init_pair(COLOR_TITLE, COLOR_WHITE, COLOR_BLUE);      // Title bar
    init_pair(COLOR_STATUS, COLOR_BLACK, COLOR_CYAN);     // Status bar
    init_pair(COLOR_NOTIFY, COLOR_WHITE, COLOR_RED);      // Notifications
    init_pair(COLOR_URL, COLOR_WHITE, COLOR_BLACK);       // URL bar
    init_pair(COLOR_CONTENT, COLOR_WHITE, COLOR_BLACK);   // Content
    init_pair(COLOR_MENU, COLOR_BLACK, COLOR_WHITE);      // Menu
    init_pair(COLOR_HIGHLIGHT, COLOR_WHITE, COLOR_GREEN); // Highlighted items
    init_pair(COLOR_WARNING, COLOR_BLACK, COLOR_YELLOW);  // Warnings
    
    // Refresh screen to ensure we have the latest terminal size
    refresh();
    
    // Create UI
    update_ui();
    update_status();
    
    // Ensure all windows are refreshed
    wrefresh(titlewin);
    wrefresh(cmdwin);
    wrefresh(contentwin);
    wrefresh(notificationwin);
    wrefresh(statuswin);
    
    // Force a full screen refresh to ensure everything is displayed
    refresh();

    // Start response listener thread
    pthread_create(&response_thread, NULL, listen_response, NULL);
    
    // Start sync thread
    pthread_create(&sync_thread, NULL, sync_thread_func, NULL);
    
    // Main event loop
    int ch;
    char input[MAX_MSG];
    
    while (running) {
        // Show menu if active
        if (show_menu) {
            display_menu();
        }
        
        // Update UI if needed
        if (notification_time > 0 && time(NULL) - notification_time > 5) {
            // Clear notification after 5 seconds
            werase(notificationwin);
            wattron(notificationwin, COLOR_PAIR(COLOR_NOTIFY));
            draw_borders(notificationwin);
            mvwprintw(notificationwin, 0, 2, " Notifications ");
            wattroff(notificationwin, COLOR_PAIR(COLOR_NOTIFY));
            wrefresh(notificationwin);
            notification_time = 0;
        }
        
        // Get input without blocking
        timeout(500); // Half-second timeout
        ch = getch();
        
        if (ch == ERR) {
            // Timeout occurred, just refresh the UI
            continue;
        }
        
        if (show_menu) {
            // Handle menu navigation
            switch (ch) {
                case KEY_UP:
                    selected_menu_item = (selected_menu_item + num_menu_items - 1) % num_menu_items;
                    display_menu();
                    break;
                    
                case KEY_DOWN:
                    selected_menu_item = (selected_menu_item + 1) % num_menu_items;
                    display_menu();
                    break;
                    
                case 10: // Enter
                    handle_menu_action();
                    break;
                    
                case 27: // ESC
                case KEY_F(1):
                    show_menu = 0;
                    break;
            }
        } else {
            // Handle normal mode
            switch (ch) {
                case KEY_F(1): // F1 - Show menu
                    show_menu = 1;
                    selected_menu_item = 0;
                    display_menu();
                    break;
                    
                case KEY_F(2): // F2 - Load
                    show_notification("Enter URL to load");
                    echo();
                    curs_set(1);
                    wmove(cmdwin, 3, 15);
                    wgetnstr(cmdwin, input, MAX_MSG - 6);
                    noecho();
                    curs_set(0);
                    
                    if (strlen(input) > 0) {
                        // Đảm bảo không tràn bộ đệm khi tạo command
                        snprintf(msg.command, sizeof(msg.command), "load %.*s",
                                (int)(sizeof(msg.command) - 6), input);
                        msg.cmd_type = CMD_LOAD;
                        strncpy(current_url, input, MAX_MSG - 1);
                        current_url[MAX_MSG - 1] = '\0';
                        update_ui();
                        if (write(write_fd, &msg, sizeof(msg)) < 0) {
                            perror("write");
                        }
                    }
                    break;
                    
                case KEY_F(3): // F3 - Reload
                    show_notification("Reloading page...");
                    strcpy(msg.command, "reload");
                    msg.cmd_type = CMD_RELOAD;
                    if (write(write_fd, &msg, sizeof(msg)) < 0) {
                        perror("write");
                    }
                    break;
                    
                case KEY_F(4): // F4 - Back
                    show_notification("Going back...");
                    strcpy(msg.command, "back");
                    msg.cmd_type = CMD_BACK;
                    if (write(write_fd, &msg, sizeof(msg)) < 0) {
                        perror("write");
                    }
                    break;
                    
                case KEY_F(5): // F5 - Forward
                    show_notification("Going forward...");
                    strcpy(msg.command, "forward");
                    msg.cmd_type = CMD_FORWARD;
                    if (write(write_fd, &msg, sizeof(msg)) < 0) {
                        perror("write");
                    }
                    break;
                    
                case KEY_F(6): // F6 - Bookmark
                    show_notification("Bookmarking current page...");
                    strcpy(msg.command, "bookmark");
                    msg.cmd_type = CMD_BOOKMARK;
                    if (write(write_fd, &msg, sizeof(msg)) < 0) {
                        perror("write");
                    }
                    break;
                    
                case KEY_F(7): // F7 - History
                    show_notification("Showing history...");
                    strcpy(msg.command, "history");
                    msg.cmd_type = CMD_HISTORY;
                    if (write(write_fd, &msg, sizeof(msg)) < 0) {
                        perror("write");
                    }
                    break;
                    
                case KEY_F(8): // F8 - Toggle Sync
                    if (is_synced) {
                        strcpy(msg.command, "sync off");
                        msg.cmd_type = CMD_SYNC_OFF;
                        is_synced = 0;
                        show_notification("Synchronization disabled");
                    } else {
                        strcpy(msg.command, "sync on");
                        msg.cmd_type = CMD_SYNC_ON;
                        is_synced = 1;
                        show_notification("Synchronization enabled");
                    }
                    update_status();
                    if (write(write_fd, &msg, sizeof(msg)) < 0) {
                        perror("write");
                    }
                    break;
                    
                case KEY_F(9): // F9 - Status
                    show_notification("Showing browser status...");
                    strcpy(msg.command, "status");
                    msg.cmd_type = CMD_STATUS;
                    if (write(write_fd, &msg, sizeof(msg)) < 0) {
                        perror("write");
                    }
                    break;
                    
                case KEY_F(10): // F10 - Exit
                    running = 0;
                    break;
                    
                case 'c': // Command mode
                    // Show command prompt
                    show_notification("Enter command");
                    echo();
                    curs_set(1);
                    werase(cmdwin);
                    wattron(cmdwin, COLOR_PAIR(COLOR_URL));
                    draw_borders(cmdwin);
                    mvwprintw(cmdwin, 0, 2, " Location ");
                    mvwprintw(cmdwin, 1, 2, "🔗 %s", current_url);
                    mvwprintw(cmdwin, 3, 2, "💻 Command > ");
                    wattroff(cmdwin, COLOR_PAIR(COLOR_URL));
                    wrefresh(cmdwin);
                    
                    wmove(cmdwin, 3, 15);
                    wgetnstr(cmdwin, input, MAX_MSG);
                    noecho();
                    curs_set(0);
                    
                    if (strcmp(input, "exit") == 0) {
                        running = 0;
                        break;
                    }
                    
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
                    
                    // Set timestamp and send command
                    msg.timestamp = time(NULL);
                    strncpy(msg.command, input, MAX_MSG);
                    if (write(write_fd, &msg, sizeof(msg)) < 0) {
                        perror("write");
                    }
                    break;
            }
        }
    }

    endwin();
    cleanup();
    close(write_fd);
    return 0;
}

