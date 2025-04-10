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

// UI Constants - Simplified for basic UI
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

// Global variable to track if a key was just pressed
int key_just_pressed = 0;

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

// Simple border drawing
void draw_borders(WINDOW *win) {
    box(win, '|', '-');
}

// Show notification that persists longer
void show_notification(const char *message) {
    if (!statuswin) return;
    
    // Clear notification line
    int rows, cols;
    getmaxyx(statuswin, rows, cols);
    wmove(statuswin, rows - 1, 1);
    wclrtoeol(statuswin);
    
    // Display new notification
    mvwprintw(statuswin, rows - 1, 2, "Message: %s", message);
    wrefresh(statuswin);
    
    // Store for persistence - show for 10 seconds instead of 5
    strncpy(notification, message, MAX_MSG - 1);
    notification[MAX_MSG - 1] = '\0';
    notification_time = time(NULL) + 10; // Show for 10 seconds
}

// Update notification based on timestamp
void check_notification() {
    if (notification[0] != '\0' && notification_time > time(NULL)) {
        int rows, cols;
        getmaxyx(statuswin, rows, cols);
        mvwprintw(statuswin, rows - 1, 2, "Message: %s", notification);
        wrefresh(statuswin);
    }
}

// Simplified UI update function
void update_ui() {
    clear();
    
    int term_rows, term_cols;
    getmaxyx(stdscr, term_rows, term_cols);
    
    // Title area - simple text
    mvprintw(0, 0, "=== Mini Browser - Tab %d ===", tab_id);
    mvprintw(1, 0, "URL: %s", current_url);
    refresh();
    
    // Content area - simple box
    contentwin = newwin(term_rows - 6, term_cols, 2, 0);
    draw_borders(contentwin);
    mvwprintw(contentwin, 0, 2, "Content:");
    wrefresh(contentwin);
    
    // Status area - simple text
    statuswin = newwin(3, term_cols, term_rows - 4, 0);
    draw_borders(statuswin);
    mvwprintw(statuswin, 0, 2, "Status: %s | Sync: %s", 
              is_connected ? "Connected" : "Disconnected", 
              is_synced ? "On" : "Off");
    mvwprintw(statuswin, 1, 2, "F1:Menu F2:Load F3:Reload F10:Exit c:Command");
    wrefresh(statuswin);
    
    // Command/notification area
    cmdwin = newwin(1, term_cols, term_rows - 1, 0);
    mvwprintw(cmdwin, 0, 0, "Command > ");
    wrefresh(cmdwin);
    
    // Simple menu window (hidden by default)
    menuwin = newwin(num_menu_items + 2, 25, 3, 5);
    
    // Set notification window
    notificationwin = statuswin;
    
    // Restore any active notification
    if (notification[0] != '\0' && notification_time > time(NULL)) {
        int rows, cols;
        getmaxyx(statuswin, rows, cols);
        mvwprintw(statuswin, rows - 1, 2, "Message: %s", notification);
        wrefresh(statuswin);
    }
    
    refresh();
}

// Simplified status update
void update_status() {
    if (!statuswin) return;
    
    mvwprintw(statuswin, 0, 2, "Status: %s | Sync: %s", 
              is_connected ? "Connected" : "Disconnected", 
              is_synced ? "On" : "Off");
    wrefresh(statuswin);
    
    // Preserve notification if it exists
    check_notification();
}

// Simplified menu display
void display_menu() {
    if (!menuwin) return;
    
    // Create a new menu window each time to ensure it's visible
    werase(menuwin);
    draw_borders(menuwin);
    mvwprintw(menuwin, 0, 2, "Menu");
    
    for (int i = 0; i < num_menu_items; i++) {
        if (i == selected_menu_item) {
            wattron(menuwin, A_REVERSE | A_BOLD); // Make selection more visible
            mvwprintw(menuwin, i + 1, 2, "-> %s", tab_menu_items[i]);
            wattroff(menuwin, A_REVERSE | A_BOLD);
        } else {
            mvwprintw(menuwin, i + 1, 2, "   %s", tab_menu_items[i]);
        }
    }
    
    // Force refresh and bring to front
    redrawwin(menuwin);
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

// Hàm lắng nghe và hiển thị nội dung
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
                response[total_read] = '\0'; // Null terminate
                
                werase(contentwin);
                draw_borders(contentwin);
                mvwprintw(contentwin, 0, 2, "Content:");
                
                int win_rows, win_cols;
                getmaxyx(contentwin, win_rows, win_cols);
                int max_line_len = win_cols - 4;
                int max_lines = win_rows - 1;
                
                int line = 1;
                char *token = strtok(response, "\n");
                while (token && line < max_lines) {
                    mvwprintw(contentwin, line++, 2, "%.80s", token);
                    token = strtok(NULL, "\n");
                }
                wrefresh(contentwin);
                
                // Update URL display
                mvprintw(1, 0, "URL: %-80s", current_url);
                refresh();
                
                update_status();
            }
        }
    }

    close(read_fd);
    return NULL;
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
            wmove(cmdwin, 0, 11);  // Position after "Command > "
            wgetnstr(cmdwin, input, MAX_MSG - 6);
            noecho();
            
            if (strlen(input) > 0) {
                snprintf(msg.command, sizeof(msg.command), "load %.*s", 
                        (int)(sizeof(msg.command) - 6), input);
                msg.cmd_type = CMD_LOAD;
                strncpy(current_url, input, MAX_MSG - 1);
                current_url[MAX_MSG - 1] = '\0';
                update_ui();
                if (write(write_fd, &msg, sizeof(msg)) < 0) {
                    perror("write");
                    show_notification("Error sending command!");
                } else {
                    show_notification("Page loading...");
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
        fprintf(stderr, "Usage: %s <tab_id>\n", argv[0]);
        return 1;
    }
    tab_id = atoi(argv[1]);
    
    printf("[Tab %d] Bat dau khoi tao...\n", tab_id);
    fflush(stdout);

    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Tạo FIFO
    snprintf(response_fifo, sizeof(response_fifo), "%s%d", RESPONSE_FIFO_PREFIX, tab_id);
    mkfifo(response_fifo, 0666);
    printf("[Tab %d] Response FIFO '%s' created.\n", tab_id, response_fifo);
    fflush(stdout);

    // Kết nối tới browser FIFO
    printf("[Tab %d] Dang ket noi toi browser...\n", tab_id);
    fflush(stdout);
    write_fd = open(BROWSER_FIFO, O_WRONLY);
    if (write_fd < 0) {
        perror("open browser fifo");
        fprintf(stderr, "[Tab %d] Loi: Khong the ket noi toi browser. Dam bao ./browser dang chay.\n", tab_id);
        unlink(response_fifo); // Xóa response fifo nếu không kết nối được
        exit(1);
    }
    is_connected = 1;
    printf("[Tab %d] Da ket noi toi browser.\n", tab_id);
    fflush(stdout);
    
    // Kết nối shared memory (không bắt buộc phải thành công ngay)
    if (init_shared_memory_connection() == 0) {
        printf("[Tab %d] Da ket noi Shared Memory.\n", tab_id);
    } else {
        printf("[Tab %d] Canh bao: Chua ket noi Shared Memory.\n", tab_id);
    }
    fflush(stdout);

    // Initialize ncurses with minimal settings
    initscr();
    if (stdscr == NULL) {
        fprintf(stderr, "[Tab %d] Error: Cannot initialize ncurses screen.\n", tab_id);
        exit(1);
    }
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    
    // Draw UI and start threads
    update_ui();
    
    // Disable fancy colors, just use basic UI
    if (has_colors()) {
        start_color();
        init_pair(1, COLOR_WHITE, COLOR_BLACK);
    }
    
    // CRITICAL: Start the response thread to receive data from browser
    if (pthread_create(&response_thread, NULL, listen_response, NULL) != 0) {
        endwin();
        fprintf(stderr, "[Tab %d] Error: Cannot create response thread.\n", tab_id);
        exit(1);
    }
    
    // Optional: Start sync thread for tab synchronization
    if (pthread_create(&sync_thread, NULL, sync_thread_func, NULL) != 0) {
        // Non-critical, just show warning
        show_notification("Warning: Sync functionality unavailable");
    } else {
        printf("[Tab %d] Sync thread started\n", tab_id);
    }
    
    // Notification that will persist
    show_notification("SIMPLIFIED UI: Testing functionality - English interface");
    
    // Main event loop
    int ch;
    BrowserMessage msg;
    msg.tab_id = tab_id;
    char input[MAX_MSG];
    
    while (running) {
        // Reset key press flag at the start of each loop
        key_just_pressed = 0;
        
        if (show_menu) {
            display_menu();
        } else {
            // Check and refresh notifications if needed
            check_notification();
            
            // Periodic refresh to keep UI stable
            touchwin(stdscr);
            refresh();
        }
        
        timeout(100); // Shorter timeout for more responsive UI
        ch = getch();
        
        // Mark that a key was pressed if not ERR
        if (ch != ERR) {
            key_just_pressed = 1;
        }
        
        // Handle menu display
        // IMPORTANT CHANGE: Only hide menu if a key is pressed AND it's not a menu navigation key
        if (show_menu && ch != ERR && ch != KEY_UP && ch != KEY_DOWN && ch != 10 && ch != 27 && ch != KEY_F(1)) {
            // Only close the menu if a non-menu key is pressed
            werase(menuwin);
            wrefresh(menuwin);
            show_menu = 0;
            update_ui(); // Redraw the UI after hiding menu
        }

        if (ch == ERR) { // Timeout
            continue;
        }
        
        if (show_menu) { // Xử lý khi menu đang hiện
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
                    // Sau khi xử lý xong, ẩn menu và vẽ lại UI chính
                    werase(menuwin);
                    wrefresh(menuwin);
                    show_menu = 0;
                    update_ui(); // Vẽ lại UI
                    refresh(); // Refresh màn hình chính
                    break;
                case 27: // ESC
                case KEY_F(1):
                    werase(menuwin);
                    wrefresh(menuwin);
                    show_menu = 0;
                    update_ui(); // Vẽ lại UI
                    refresh(); // Refresh màn hình chính
                    break;
            }
        } else { // Xử lý khi không ở trong menu
            switch (ch) {
                case KEY_F(1): // F1 - Show menu
                    show_menu = 1;
                    selected_menu_item = 0;
                    // Clear any existing menu and draw fresh
                    if (menuwin) delwin(menuwin);
                    menuwin = newwin(num_menu_items + 2, 30, 3, 5);
                    display_menu();
                    show_notification("Menu displayed - Use arrow keys to navigate");
                    break;
                case KEY_F(2): // F2 - Load (Dùng chế độ nhập lệnh)
                case 'c':      // c - Command mode
                    // Clear the command line
                    werase(cmdwin);
                    mvwprintw(cmdwin, 0, 0, "Command > ");
                    wrefresh(cmdwin);
                    
                    echo(); curs_set(1); // Show cursor and typing
                    wmove(cmdwin, 0, 10);
                    
                    wgetnstr(cmdwin, input, sizeof(input) - 1);
                    
                    noecho(); curs_set(0);
                    
                    if (strlen(input) > 0) {
                        // Process command
                        if (strcmp(input, "exit") == 0) {
                            running = 0; 
                            break;
                        }
                        
                        msg.timestamp = time(NULL);
                        strncpy(msg.command, input, sizeof(msg.command) - 1);
                        msg.command[sizeof(msg.command) - 1] = '\0';
                        
                        // CRITICAL: Set command type based on input
                        if (strncmp(input, "load ", 5) == 0) {
                            strncpy(current_url, input + 5, sizeof(current_url) - 1);
                            current_url[sizeof(current_url) - 1] = '\0';
                            update_ui(); // Update URL on UI
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
                            is_synced = 1; update_status();
                        } else if (strcmp(input, "sync off") == 0) {
                            msg.cmd_type = CMD_SYNC_OFF;
                            is_synced = 0; update_status();
                        } else if (strcmp(input, "status") == 0) {
                            msg.cmd_type = CMD_STATUS;
                        } else {
                            msg.cmd_type = CMD_UNKNOWN;
                        }
                        
                        // Send command to browser
                        if (write(write_fd, &msg, sizeof(msg)) < 0) {
                            perror("write to browser");
                            show_notification("Error sending command!");
                        } else {
                            char notification_text[MAX_MSG];
                            snprintf(notification_text, MAX_MSG, "Command sent: %s", input);
                            show_notification(notification_text);
                        }
                    }
                    
                    // Restore the command line
                    werase(cmdwin);
                    mvwprintw(cmdwin, 0, 0, "Command > ");
                    wrefresh(cmdwin);
                    break;

                case KEY_F(3): // F3 - Reload
                    show_notification("Reloading page...");
                    strcpy(msg.command, "reload");
                    msg.cmd_type = CMD_RELOAD;
                    if (write(write_fd, &msg, sizeof(msg)) < 0) perror("write reload");
                    break;
                    
                // Các phím F khác có thể thêm tương tự hoặc để trong menu F1
                
                case KEY_F(10): // F10 - Exit
                    running = 0;
                    break;
                    
                 // Thêm xử lý thay đổi kích thước màn hình nếu cần
                case KEY_RESIZE:
                    update_ui(); // Vẽ lại UI khi thay đổi kích thước
                    refresh(); // Refresh màn hình chính
                    break;
            }
        }
        
        // Always refresh the UI after processing input
        update_status();
    }

    // Dọn dẹp trước khi thoát
    endwin(); // Kết thúc chế độ ncurses trước khi printf
    printf("[Tab %d] Dang don dep...\n", tab_id);
    fflush(stdout);
    cleanup(); 
    close(write_fd);
    printf("[Tab %d] Da thoat.\n", tab_id);
    return 0;
}

