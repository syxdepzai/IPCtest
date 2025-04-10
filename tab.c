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

// Hàm vẽ viền siêu đơn giản
void draw_basic_border(WINDOW *win) {
    box(win, 0, 0); // Dùng box() của ncurses cho đơn giản
}

// Hàm hiển thị thông báo trên dòng cuối cùng của statuswin
void show_notification(const char *message) {
    if (!statuswin) return;
    
    int rows, cols;
    getmaxyx(statuswin, rows, cols);
    
    // Xóa dòng cuối (dòng thông báo)
    wmove(statuswin, rows - 1, 1);
    wclrtoeol(statuswin);
    
    // Hiển thị thông báo mới, cắt bớt nếu quá dài
    char temp_msg[cols - 4]; // Buffer tạm
    snprintf(temp_msg, sizeof(temp_msg), "Thông báo: %s", message);
    mvwprintw(statuswin, rows - 1, 2, "%.*s", cols - 4, temp_msg);
    
    wrefresh(statuswin);
}

// Hàm cập nhật giao diện siêu đơn giản
void update_ui() {
    clear(); // Xóa toàn bộ màn hình trước
    
    int term_rows, term_cols;
    getmaxyx(stdscr, term_rows, term_cols);
    
    // 1. Title Bar (2 dòng)
    int title_height = 2;
    titlewin = newwin(title_height, term_cols, 0, 0);
    draw_basic_border(titlewin);
    mvwprintw(titlewin, 0, 2, "Tab %d", tab_id);
    mvwprintw(titlewin, 1, 2, "URL: %.*s", term_cols - 8, current_url); // Cắt URL nếu dài
    wrefresh(titlewin);
    
    // 2. Status Bar (3 dòng: status, keys, input/notify)
    int status_height = 3;
    statuswin = newwin(status_height, term_cols, term_rows - status_height, 0);
    draw_basic_border(statuswin);
    // Dòng trạng thái
    int connect_status = is_connected ? 1 : 0;
    int sync_status = is_synced ? 1 : 0;
    mvwprintw(statuswin, 0, 2, "Trạng thái: %s | Đồng bộ: %s", 
              connect_status ? "OK" : "--", 
              sync_status ? "Bật" : "Tắt");
    // Dòng phím tắt
    mvwprintw(statuswin, 1, 2, "F1:Menu F2:Load F3:Reload F10:Exit c:Lệnh");
    // Dòng thông báo/nhập lệnh (sẽ được cập nhật bởi show_notification hoặc phím 'c')
    mvwprintw(statuswin, 2, 2, "Thông báo: Sẵn sàng.");
    wrefresh(statuswin);
    
    // 3. Content Window (chiếm phần còn lại)
    int content_height = term_rows - title_height - status_height;
    if (content_height < 1) content_height = 1; // Ít nhất 1 dòng
    contentwin = newwin(content_height, term_cols, title_height, 0);
    draw_basic_border(contentwin);
    mvwprintw(contentwin, 0, 2, "Nội dung");
    wrefresh(contentwin);
    
    // Tạo cửa sổ menu (không vẽ ngay)
    menuwin = newwin(num_menu_items + 2, 25, 2, 2); // Đặt gần title bar
    
    // Đặt notificationwin = statuswin để hàm show_notification hoạt động
    notificationwin = statuswin;
    
    refresh(); // Refresh toàn bộ màn hình sau khi vẽ các cửa sổ
}

// Hàm cập nhật thanh trạng thái (chỉ dòng đầu)
void update_status() {
    if (!statuswin) return;
    
    int rows, cols;
    getmaxyx(statuswin, rows, cols);
    
    // Xóa dòng trạng thái (dòng 0)
    wmove(statuswin, 0, 1);
    wclrtoeol(statuswin);
    
    // Vẽ lại trạng thái
    int connect_status = is_connected ? 1 : 0;
    int sync_status = is_synced ? 1 : 0;
    mvwprintw(statuswin, 0, 2, "Trạng thái: %s | Đồng bộ: %s", 
              connect_status ? "OK" : "--", 
              sync_status ? "Bật" : "Tắt");
    wrefresh(statuswin);
}

// Hàm hiển thị menu đơn giản
void display_menu() {
    if (!menuwin) return;
    werase(menuwin);
    draw_basic_border(menuwin);
    mvwprintw(menuwin, 0, 2, "Menu");
    for (int i = 0; i < num_menu_items; i++) {
        if (i == selected_menu_item) {
            wattron(menuwin, A_REVERSE); // Highlight dòng được chọn
            mvwprintw(menuwin, i + 1, 2, "> %s", tab_menu_items[i]);
            wattroff(menuwin, A_REVERSE);
        } else {
            mvwprintw(menuwin, i + 1, 2, "  %s", tab_menu_items[i]);
        }
    }
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
                draw_basic_border(contentwin);
                mvwprintw(contentwin, 0, 2, "Nội dung");
                
                int win_rows, win_cols;
                getmaxyx(contentwin, win_rows, win_cols);
                int max_line_len = win_cols - 4;
                int max_lines = win_rows - 1; // Để lại dòng tiêu đề "Nội dung"
                
                int line = 1;
                char *token = strtok(response, "\n");
                while (token && line < max_lines) {
                    mvwprintw(contentwin, line++, 2, "%.*s", max_line_len, token); // Cắt nếu dài
                    token = strtok(NULL, "\n");
                }
                wrefresh(contentwin);
                
                // Cập nhật URL trên title bar
                if (current_url[0] != '\0') {
                    int title_rows, title_cols;
                    getmaxyx(titlewin, title_rows, title_cols);
                    wmove(titlewin, 1, 1);
                    wclrtoeol(titlewin);
                    mvwprintw(titlewin, 1, 2, "URL: %.*s", title_cols - 8, current_url);
                    wrefresh(titlewin);
                }
                update_status(); // Cập nhật trạng thái
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
        fprintf(stderr, "Usage: %s <tab_id>\n", argv[0]);
        return 1;
    }
    tab_id = atoi(argv[1]);
    
    printf("[Tab %d] Bắt đầu khởi tạo...
", tab_id);
    fflush(stdout);

    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Tạo FIFO
    snprintf(response_fifo, sizeof(response_fifo), "%s%d", RESPONSE_FIFO_PREFIX, tab_id);
    mkfifo(response_fifo, 0666);
    printf("[Tab %d] Response FIFO '%s' created.
", tab_id, response_fifo);
    fflush(stdout);

    // Kết nối tới browser FIFO
    printf("[Tab %d] Đang kết nối tới browser...
", tab_id);
    fflush(stdout);
    write_fd = open(BROWSER_FIFO, O_WRONLY);
    if (write_fd < 0) {
        perror("open browser fifo");
        fprintf(stderr, "[Tab %d] Lỗi: Không thể kết nối tới browser. Đảm bảo ./browser đang chạy.
", tab_id);
        unlink(response_fifo); // Xóa response fifo nếu không kết nối được
        exit(1);
    }
    is_connected = 1;
    printf("[Tab %d] Đã kết nối tới browser.
", tab_id);
    fflush(stdout);
    
    // Kết nối shared memory (không bắt buộc phải thành công ngay)
    if (init_shared_memory_connection() == 0) {
        printf("[Tab %d] Đã kết nối Shared Memory.
", tab_id);
    } else {
        printf("[Tab %d] Cảnh báo: Chưa kết nối Shared Memory.
", tab_id);
    }
    fflush(stdout);

    // Khởi tạo ncurses
    printf("[Tab %d] Đang khởi tạo Ncurses UI...
", tab_id);
    fflush(stdout);
    initscr();            // Bắt đầu chế độ ncurses
    if (stdscr == NULL) {
        fprintf(stderr, "[Tab %d] Lỗi: Không thể khởi tạo màn hình ncurses.
", tab_id);
        exit(1);
    }
    refresh();            // Vẽ màn hình lần đầu (có thể trống)
    start_color();        // Bật màu
    cbreak();             // Tắt buffer dòng, nhận ký tự ngay
    noecho();             // Không hiện ký tự gõ ra màn hình
    keypad(stdscr, TRUE); // Bật chế độ nhận phím đặc biệt (F1, mũi tên...)
    curs_set(0);          // Ẩn con trỏ

    // Kiểm tra kích thước terminal (vẫn giữ để tránh lỗi vẽ)
    int min_rows = 10; // Giảm yêu cầu xuống tối thiểu
    int min_cols = 50;
    int term_rows, term_cols;
    getmaxyx(stdscr, term_rows, term_cols);
    if (term_rows < min_rows || term_cols < min_cols) {
        endwin();
        fprintf(stderr, "[Tab %d] Lỗi: Terminal quá nhỏ (%dx%d). Yêu cầu tối thiểu: %dx%d.
", 
               tab_id, term_cols, term_rows, min_cols, min_rows);
        exit(1);
    }
    
    // Định nghĩa màu cơ bản (nếu cần)
    if (has_colors()) {
        init_pair(1, COLOR_WHITE, COLOR_BLUE);  // Title
        init_pair(2, COLOR_BLACK, COLOR_CYAN);  // Status
        // Không cần nhiều màu cho giao diện tối giản
    }

    printf("[Tab %d] Ncurses đã khởi tạo. Đang vẽ UI...
", tab_id);
    fflush(stdout);
    
    // Vẽ giao diện lần đầu
    update_ui(); 
    printf("[Tab %d] UI đã vẽ. Bắt đầu các luồng...
", tab_id);
    fflush(stdout);

    // Bắt đầu các luồng sau khi UI đã sẵn sàng
    if (pthread_create(&response_thread, NULL, listen_response, NULL) != 0) {
        perror("pthread_create response_thread");
        endwin(); exit(1);
    }
    if (pthread_create(&sync_thread, NULL, sync_thread_func, NULL) != 0) {
        perror("pthread_create sync_thread");
        endwin(); exit(1);
    }
    printf("[Tab %d] Các luồng đã bắt đầu. Sẵn sàng nhận lệnh.
", tab_id);
    fflush(stdout);

    // Main event loop
    int ch;
    BrowserMessage msg; // Khai báo msg ở đây
    msg.tab_id = tab_id; // Gán tab_id một lần
    char input[MAX_MSG]; // Buffer cho lệnh nhập
    
    while (running) {
        if (show_menu) {
            display_menu();
        } 
        
        // Lấy input (vẫn dùng timeout để không block hoàn toàn)
        timeout(500); // Timeout nửa giây
        ch = getch();
        
        // Xóa menu nếu người dùng nhập gì đó khác điều hướng menu
        if (show_menu && ch != KEY_UP && ch != KEY_DOWN && ch != 10) {
             werase(menuwin);
             wrefresh(menuwin);
             show_menu = 0;
             update_ui(); // Vẽ lại UI chính sau khi ẩn menu
        }

        if (ch == ERR) { // Nếu timeout
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
                    update_ui();
                    break;
                case 27: // ESC
                case KEY_F(1):
                    werase(menuwin);
                    wrefresh(menuwin);
                    show_menu = 0;
                    update_ui();
                    break;
            }
        } else { // Xử lý khi không ở trong menu
            switch (ch) {
                case KEY_F(1): // F1 - Show menu
                    show_menu = 1;
                    selected_menu_item = 0;
                    display_menu();
                    break;
                case KEY_F(2): // F2 - Load (Dùng chế độ nhập lệnh)
                case 'c':      // c - Command mode
                    // Sử dụng dòng cuối của statuswin để nhập lệnh
                    wmove(statuswin, 2, 1);
                    wclrtoeol(statuswin);
                    mvwprintw(statuswin, 2, 2, "Lệnh > ");
                    wrefresh(statuswin);
                    
                    echo(); curs_set(1); // Bật echo và con trỏ
                    wmove(statuswin, 2, 9); // Di chuyển con trỏ đến vị trí nhập
                    
                    wgetnstr(statuswin, input, sizeof(input) - 1);
                    
                    noecho(); curs_set(0); // Tắt echo và con trỏ
                    
                    // Xóa dòng nhập lệnh sau khi nhập xong
                    wmove(statuswin, 2, 1);
                    wclrtoeol(statuswin);
                    mvwprintw(statuswin, 2, 2, "Thông báo: Đang xử lý...");
                    wrefresh(statuswin);
                    
                    if (strlen(input) == 0) break; // Bỏ qua nếu không nhập gì
                    
                    if (strcmp(input, "exit") == 0) {
                        running = 0; break;
                    }
                    
                    // Phân tích và gửi lệnh
                    msg.timestamp = time(NULL);
                    strncpy(msg.command, input, sizeof(msg.command) - 1);
                    msg.command[sizeof(msg.command) - 1] = '\0'; // Ensure null termination
                    
                    // Xác định loại lệnh (có thể gộp vào browser.c nếu muốn)
                    if (strncmp(input, "load ", 5) == 0) {
                        strncpy(current_url, input + 5, sizeof(current_url) - 1);
                        current_url[sizeof(current_url) - 1] = '\0';
                        update_ui(); // Cập nhật URL trên UI
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
                    
                    // Gửi lệnh tới browser
                    if (write(write_fd, &msg, sizeof(msg)) < 0) {
                        perror("write to browser");
                        show_notification("Lỗi gửi lệnh!");
                    } else {
                        show_notification("Đã gửi lệnh: ");
                        waddstr(statuswin, input); // Hiển thị lại lệnh đã gửi
                        wrefresh(statuswin);
                    }
                    break;

                case KEY_F(3): // F3 - Reload (Gửi lệnh reload)
                    show_notification("Đang tải lại...");
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
                    break;
            }
        }
    }

    // Dọn dẹp trước khi thoát
    printf("[Tab %d] Đang dọn dẹp...
", tab_id);
    fflush(stdout);
    endwin(); // Kết thúc chế độ ncurses
    cleanup(); // Dọn dẹp FIFO, shared memory
    close(write_fd); // Đóng kết nối tới browser
    printf("[Tab %d] Đã thoát.
", tab_id);
    return 0;
}

