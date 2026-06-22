#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define MAX_COLS 300
#define DEFAULT_ROWS 24
#define DEFAULT_COLS 80
#define FRAME_INTERVAL_MS 60

/* 保存终端状态，确保程序退出后 shell 仍能正常显示和接收输入。 */
static struct termios original_termios;
static int termios_changed = 0;
static int original_stdin_flags = -1;
static int stdin_flags_changed = 0;
static int stdin_polling_enabled = 0;
static volatile sig_atomic_t should_exit = 0;
static int cleanup_finished = 0;

static void sleep_ms(int ms) {
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* 信号处理函数中只修改标志，终端恢复工作留在正常流程中完成。 */
static void request_exit(int signal_number) {
    (void)signal_number;
    should_exit = 1;
}

static void restore_terminal(void) {
    if (cleanup_finished) {
        return;
    }
    cleanup_finished = 1;

    if (termios_changed) {
        tcsetattr(STDIN_FILENO, TCSANOW, &original_termios);
    }
    if (stdin_flags_changed) {
        fcntl(STDIN_FILENO, F_SETFL, original_stdin_flags);
    }

    /* 恢复颜色和光标，并清理动画画面，避免 shell 看起来像是“坏掉了”。 */
    printf("\033[0m\033[?25h\033[2J\033[H");
    printf("Cyber rain stopped.\n");
    fflush(stdout);
}

/*
 * 关闭规范输入和回显后，按键不需要等待回车即可被 read() 读取。
 * 返回 1 表示可直接按 q；返回 0 时仍可使用 q+回车或 Ctrl+C。
 */
static int configure_keyboard(void) {
    struct termios raw_termios;
    int raw_input_enabled = 0;

    if (tcgetattr(STDIN_FILENO, &original_termios) == 0) {
        raw_termios = original_termios;
        raw_termios.c_lflag &= (tcflag_t)~(ICANON | ECHO);
        raw_termios.c_cc[VMIN] = 0;
        raw_termios.c_cc[VTIME] = 0;

        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw_termios) == 0) {
            termios_changed = 1;
            raw_input_enabled = 1;
            stdin_polling_enabled = 1;
        }
    }

    original_stdin_flags = fcntl(STDIN_FILENO, F_GETFL);
    if (original_stdin_flags >= 0 &&
        fcntl(STDIN_FILENO, F_SETFL, original_stdin_flags | O_NONBLOCK) == 0) {
        stdin_flags_changed = 1;
        stdin_polling_enabled = 1;
    }

    return raw_input_enabled;
}

static int stdout_is_unsafe_target(void) {
    struct stat status;

    return fstat(STDOUT_FILENO, &status) == 0 &&
           (S_ISREG(status.st_mode) || S_ISBLK(status.st_mode));
}

static void check_quit_key(void) {
    char input[32];
    ssize_t count;

    if (!stdin_polling_enabled) {
        return;
    }

    do {
        count = read(STDIN_FILENO, input, sizeof(input));
        for (ssize_t i = 0; i < count; ++i) {
            if (input[i] == 'q' || input[i] == 'Q') {
                should_exit = 1;
                return;
            }
        }
    } while (count > 0);
}

int main(void) {
    struct winsize window_size;
    int rows = DEFAULT_ROWS;
    int cols = DEFAULT_COLS;
    int raw_input_enabled;

    /*
     * 动画会持续产生大量输出。若 stdout 被重定向到普通文件，文件可能不断
     * 增长并占满磁盘，因此直接拒绝这种运行方式。
     */
    if (stdout_is_unsafe_target()) {
        fprintf(stderr,
                "Refusing to run: stdout points to a file or block device.\n");
        return EXIT_FAILURE;
    }

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &window_size) == 0) {
        if (window_size.ws_row >= 2) {
            rows = window_size.ws_row;
        }
        if (window_size.ws_col > 0) {
            cols = window_size.ws_col;
        }
    }

    if (cols > MAX_COLS) {
        cols = MAX_COLS;
    }
    if (cols < 1) {
        cols = 1;
    }

    int matrix_rows = rows - 1;
    int drops[MAX_COLS];

    srand((unsigned int)time(NULL));
    for (int i = 0; i < cols; ++i) {
        drops[i] = rand() % matrix_rows;
    }

    atexit(restore_terminal);
    signal(SIGINT, request_exit);
    signal(SIGTERM, request_exit);
#ifdef SIGHUP
    signal(SIGHUP, request_exit);
#endif
    raw_input_enabled = configure_keyboard();

    printf("\033[2J");
    printf("\033[?25l");

    while (!should_exit) {
        const char *quit_prompt;

        if (raw_input_enabled) {
            quit_prompt = "Press q to exit";
        } else if (stdin_polling_enabled) {
            quit_prompt = "Press q + Enter to exit";
        } else {
            quit_prompt = "Press Ctrl+C to exit";
        }

        check_quit_key();
        if (should_exit) {
            break;
        }

        printf("\033[H");
        printf("\033[1;37m%-*.*s\033[0m\n", cols, cols, quit_prompt);

        for (int y = 0; y < matrix_rows; ++y) {
            for (int x = 0; x < cols; ++x) {
                if (drops[x] == y) {
                    char c = (char)(33 + rand() % 94);
                    printf("\033[1;32m%c\033[0m", c);
                } else if (drops[x] - 1 == y) {
                    char c = (char)(33 + rand() % 94);
                    printf("\033[0;32m%c\033[0m", c);
                } else if (drops[x] - 2 == y) {
                    char c = (char)(33 + rand() % 94);
                    printf("\033[0;2;32m%c\033[0m", c);
                } else {
                    printf(" ");
                }
            }
            printf("\n");
        }

        for (int i = 0; i < cols; ++i) {
            ++drops[i];
            if (drops[i] > matrix_rows + rand() % 20) {
                drops[i] = 0;
            }
        }

        fflush(stdout);
        sleep_ms(FRAME_INTERVAL_MS);
    }

    return EXIT_SUCCESS;
}
