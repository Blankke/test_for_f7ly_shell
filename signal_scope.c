/*
 * 用法示例：
 *   cc -O2 -Wall -Wextra -o signal_scope signal_scope.c
 *   ./signal_scope
 *
 * 说明：
 *   这个程序 fork 一个子进程作为信号发生器。子进程交替向父进程发送
 *   SIGUSR1 和 SIGUSR2，父进程在信号处理函数里计数，并在终端实时显示。
 *   它适合观察 kill/getpid/fork/waitpid 和用户态 signal handler 是否工作。
 *   按 q 退出，或按 Ctrl-C 触发 SIGINT 清理退出。
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_COLS 80

static struct termios original_termios;
static int termios_changed = 0;
static int original_stdin_flags = -1;
static int stdin_flags_changed = 0;
static volatile sig_atomic_t should_exit = 0;
static volatile sig_atomic_t child_should_exit = 0;
static volatile sig_atomic_t usr1_count = 0;
static volatile sig_atomic_t usr2_count = 0;
static volatile sig_atomic_t int_count = 0;
static int cleanup_finished = 0;
static pid_t signal_child = -1;

static void sleep_ms(int ms) {
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static void handle_usr1(int signal_number) {
    (void)signal_number;
    usr1_count++;
}

static void handle_usr2(int signal_number) {
    (void)signal_number;
    usr2_count++;
}

static void request_exit(int signal_number) {
    if (signal_number == SIGINT) {
        int_count++;
    }
    should_exit = 1;
}

static void request_child_exit(int signal_number) {
    (void)signal_number;
    child_should_exit = 1;
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
    printf("Signal scope stopped. SIGUSR1=%d SIGUSR2=%d SIGINT=%d\n",
           (int)usr1_count, (int)usr2_count, (int)int_count);
    fflush(stdout);
}

static int stdout_is_unsafe_target(void) {
    struct stat status;

    return fstat(STDOUT_FILENO, &status) == 0 &&
           (S_ISREG(status.st_mode) || S_ISBLK(status.st_mode));
}

static int terminal_cols(void) {
    struct winsize window_size;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &window_size) == 0 &&
        window_size.ws_col > 0) {
        return window_size.ws_col;
    }
    return DEFAULT_COLS;
}

static void configure_keyboard(void) {
    struct termios raw_termios;

    if (tcgetattr(STDIN_FILENO, &original_termios) == 0) {
        raw_termios = original_termios;
        raw_termios.c_lflag &= (tcflag_t)~(ICANON | ECHO);
        raw_termios.c_cc[VMIN] = 0;
        raw_termios.c_cc[VTIME] = 0;

        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw_termios) == 0) {
            termios_changed = 1;
        }
    }

    original_stdin_flags = fcntl(STDIN_FILENO, F_GETFL);
    if (original_stdin_flags >= 0) {
        if (fcntl(STDIN_FILENO, F_SETFL, original_stdin_flags | O_NONBLOCK) == 0) {
            stdin_flags_changed = 1;
        }
    }
}

static void poll_keyboard(void) {
    char input[32];
    ssize_t count;

    do {
        count = read(STDIN_FILENO, input, sizeof(input));
        if (count > 0) {
            for (ssize_t i = 0; i < count; ++i) {
                if (input[i] == 'q' || input[i] == 'Q') {
                    should_exit = 1;
                    return;
                }
            }
        }
    } while (count > 0);
}

static void child_signal_loop(pid_t parent_pid) {
    int tick = 0;

    signal(SIGTERM, request_child_exit);
    signal(SIGINT, request_child_exit);

    while (!child_should_exit) {
        int sig = (tick % 2 == 0) ? SIGUSR1 : SIGUSR2;

        if (kill(parent_pid, sig) != 0 && errno != EINTR) {
            break;
        }
        tick++;
        sleep_ms(180);
    }

    _exit(0);
}

static void draw_meter(const char *label, int value, int width, const char *color) {
    int filled;

    if (width < 10) {
        width = 10;
    }
    filled = value % (width + 1);

    printf("%-8s %4d [", label, value);
    for (int i = 0; i < width; ++i) {
        if (i < filled) {
            printf("%s#\033[0m", color);
        } else {
            putchar('.');
        }
    }
    printf("]\n");
}

int main(void) {
    int frame = 0;

    if (stdout_is_unsafe_target()) {
        fprintf(stderr, "Refusing to run: stdout points to a file or block device.\n");
        return EXIT_FAILURE;
    }

    signal(SIGUSR1, handle_usr1);
    signal(SIGUSR2, handle_usr2);
    signal(SIGINT, request_exit);
    signal(SIGTERM, request_exit);
#ifdef SIGHUP
    signal(SIGHUP, request_exit);
#endif
    atexit(restore_terminal);
    configure_keyboard();

    signal_child = fork();
    if (signal_child < 0) {
        perror("fork");
        return EXIT_FAILURE;
    }
    if (signal_child == 0) {
        child_signal_loop(getppid());
    }

    printf("\033[2J\033[?25l");
    while (!should_exit) {
        int cols = terminal_cols();
        int meter_width = cols - 24;

        if (meter_width < 16) {
            meter_width = 16;
        }
        if (meter_width > 70) {
            meter_width = 70;
        }

        poll_keyboard();
        if (waitpid(signal_child, NULL, WNOHANG) == signal_child) {
            signal_child = -1;
            should_exit = 1;
        }

        printf("\033[H");
        printf("\033[1;37mSignal scope: child kill() -> parent signal handler\033[0m\n");
        printf("parent pid=%ld child pid=%ld | press q to exit | frame=%d\n",
               (long)getpid(), (long)signal_child, frame);
        draw_meter("SIGUSR1", (int)usr1_count, meter_width, "\033[1;32m");
        draw_meter("SIGUSR2", (int)usr2_count, meter_width, "\033[1;36m");
        draw_meter("SIGINT", (int)int_count, meter_width, "\033[1;31m");
        printf("\033[J");
        fflush(stdout);

        frame++;
        sleep_ms(90);
    }

    if (signal_child > 0) {
        kill(signal_child, SIGTERM);
        waitpid(signal_child, NULL, 0);
    }

    return EXIT_SUCCESS;
}
