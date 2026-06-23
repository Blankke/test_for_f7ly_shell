/*
 * 用法示例：
 *   cc -O2 -Wall -Wextra -o tty_probe tty_probe.c
 *   ./tty_probe
 *
 * 说明：
 *   这个程序用于观察终端输入语义。它会把 stdin 切到非规范模式，
 *   设置 VMIN=0、VTIME=0，并打开 O_NONBLOCK。正常情况下，没有按键时
 *   read() 应该立即返回 EAGAIN；按任意键会显示按键码，按 q 退出。
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_ROWS 24
#define DEFAULT_COLS 80

static struct termios original_termios;
static int termios_changed = 0;
static int original_stdin_flags = -1;
static int stdin_flags_changed = 0;
static volatile sig_atomic_t should_exit = 0;
static int cleanup_finished = 0;

static void sleep_ms(int ms) {
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

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

    printf("\033[0m\033[?25h\033[2J\033[H");
    printf("TTY probe stopped.\n");
    fflush(stdout);
}

static int stdout_is_unsafe_target(void) {
    struct stat status;

    return fstat(STDOUT_FILENO, &status) == 0 &&
           (S_ISREG(status.st_mode) || S_ISBLK(status.st_mode));
}

static void get_terminal_size(int *rows, int *cols) {
    struct winsize window_size;

    *rows = DEFAULT_ROWS;
    *cols = DEFAULT_COLS;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &window_size) == 0) {
        if (window_size.ws_row > 0) {
            *rows = window_size.ws_row;
        }
        if (window_size.ws_col > 0) {
            *cols = window_size.ws_col;
        }
    }
}

static int configure_keyboard(void) {
    struct termios raw_termios;
    int configured = 0;

    if (tcgetattr(STDIN_FILENO, &original_termios) == 0) {
        raw_termios = original_termios;
        raw_termios.c_lflag &= (tcflag_t)~(ICANON | ECHO);
        raw_termios.c_cc[VMIN] = 0;
        raw_termios.c_cc[VTIME] = 0;

        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw_termios) == 0) {
            termios_changed = 1;
            configured = 1;
        }
    }

    original_stdin_flags = fcntl(STDIN_FILENO, F_GETFL);
    if (original_stdin_flags >= 0 &&
        fcntl(STDIN_FILENO, F_SETFL, original_stdin_flags | O_NONBLOCK) == 0) {
        stdin_flags_changed = 1;
        configured = 1;
    }

    return configured;
}

static void render_bar(int frame, int width) {
    int usable_width = width - 12;
    int cursor;

    if (usable_width < 10) {
        usable_width = 10;
    }
    cursor = frame % usable_width;

    printf("poll loop  [");
    for (int i = 0; i < usable_width; ++i) {
        if (i == cursor) {
            printf("\033[1;32m#\033[0m");
        } else if ((i + frame) % 7 == 0) {
            printf("\033[0;2;32m+\033[0m");
        } else {
            putchar('.');
        }
    }
    printf("]\n");
}

int main(void) {
    long eagain_reads = 0;
    long zero_reads = 0;
    long byte_reads = 0;
    long other_errors = 0;
    int last_errno = 0;
    int last_key = -1;
    int rows = DEFAULT_ROWS;
    int cols = DEFAULT_COLS;
    int configured;
    int frame = 0;

    if (stdout_is_unsafe_target()) {
        fprintf(stderr, "Refusing to run: stdout points to a file or block device.\n");
        return EXIT_FAILURE;
    }

    signal(SIGINT, request_exit);
    signal(SIGTERM, request_exit);
#ifdef SIGHUP
    signal(SIGHUP, request_exit);
#endif
    atexit(restore_terminal);
    configured = configure_keyboard();

    printf("\033[2J\033[?25l");
    while (!should_exit) {
        char input[64];
        ssize_t count;

        do {
            errno = 0;
            count = read(STDIN_FILENO, input, sizeof(input));
            if (count > 0) {
                byte_reads += count;
                for (ssize_t i = 0; i < count; ++i) {
                    last_key = (unsigned char)input[i];
                    if (input[i] == 'q' || input[i] == 'Q') {
                        should_exit = 1;
                    }
                }
            } else if (count == 0) {
                zero_reads++;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                eagain_reads++;
                last_errno = errno;
            } else if (errno == EINTR) {
                last_errno = errno;
            } else {
                other_errors++;
                last_errno = errno;
            }
        } while (count > 0 && !should_exit);

        if (should_exit) {
            break;
        }

        get_terminal_size(&rows, &cols);
        if (cols < 50) {
            cols = 50;
        }

        printf("\033[H");
        printf("\033[1;37mTTY probe: termios + O_NONBLOCK + VMIN/VTIME\033[0m\n");
        printf("terminal size : rows=%d cols=%d\n", rows, cols);
        printf("stdin mode    : ICANON=off ECHO=off VMIN=0 VTIME=0 O_NONBLOCK=on\n");
        printf("setup result  : %s\n", configured ? "ok" : "partial/fallback");
        printf("empty read    : EAGAIN=%ld zero=%ld other_error=%ld last_errno=%d(%s)\n",
               eagain_reads, zero_reads, other_errors, last_errno,
               last_errno == 0 ? "none" : strerror(last_errno));
        if (last_key >= 0) {
            printf("last key      : dec=%d hex=0x%02x char='%c'\n",
                   last_key, last_key,
                   last_key >= 32 && last_key <= 126 ? last_key : '.');
        } else {
            printf("last key      : none yet\n");
        }
        printf("bytes read    : %ld\n", byte_reads);
        printf("operation     : press any key to update, press q to exit\n");
        render_bar(frame, cols);
        printf("\033[J");
        fflush(stdout);

        frame++;
        sleep_ms(80);
    }

    return EXIT_SUCCESS;
}
