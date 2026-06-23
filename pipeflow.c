/*
 * 用法示例：
 *   cc -O2 -Wall -Wextra -o pipeflow pipeflow.c
 *   ./pipeflow
 *
 * 说明：
 *   这个程序会 fork 出多个生产者子进程。子进程不断向 pipe 写入事件，
 *   父进程把读端设置成 O_NONBLOCK，并把事件渲染成终端里的“流水线”。
 *   它可以同时观察 fork/waitpid、pipe、非阻塞 read、kill(SIGTERM)
 *   和 ANSI 终端刷新效果。按 q 退出。
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define WORKER_COUNT 4
#define DEFAULT_COLS 80
#define MIN_LANE_WIDTH 24
#define MAX_LANE_WIDTH 120

typedef struct {
    int worker;
    int tick;
    int value;
} PipeEvent;

static struct termios original_termios;
static int termios_changed = 0;
static int original_stdin_flags = -1;
static int stdin_flags_changed = 0;
static volatile sig_atomic_t should_exit = 0;
static volatile sig_atomic_t child_should_exit = 0;
static int cleanup_finished = 0;
static pid_t workers[WORKER_COUNT];
static int worker_alive[WORKER_COUNT];

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
    printf("Pipeflow stopped.\n");
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

static void producer_loop(int write_fd, int worker_id) {
    int tick = 0;

    signal(SIGTERM, request_child_exit);
    signal(SIGINT, request_child_exit);

    while (!child_should_exit) {
        PipeEvent event;

        event.worker = worker_id;
        event.tick = tick;
        event.value = (worker_id * 17 + tick * 7) % 100;
        if (write(write_fd, &event, sizeof(event)) < 0 && errno != EINTR) {
            break;
        }

        tick++;
        sleep_ms(55 + worker_id * 35);
    }

    close(write_fd);
    _exit(0);
}

static int spawn_workers(int write_fd) {
    for (int i = 0; i < WORKER_COUNT; ++i) {
        pid_t pid = fork();
        if (pid < 0) {
            return -1;
        }
        if (pid == 0) {
            producer_loop(write_fd, i);
        }
        workers[i] = pid;
        worker_alive[i] = 1;
    }
    return 0;
}

static void stop_workers(void) {
    for (int i = 0; i < WORKER_COUNT; ++i) {
        if (worker_alive[i] && workers[i] > 0) {
            kill(workers[i], SIGTERM);
        }
    }
    for (int i = 0; i < WORKER_COUNT; ++i) {
        if (worker_alive[i] && workers[i] > 0) {
            waitpid(workers[i], NULL, 0);
            worker_alive[i] = 0;
        }
    }
}

static void update_worker_status(void) {
    for (int i = 0; i < WORKER_COUNT; ++i) {
        int status = 0;

        if (!worker_alive[i] || workers[i] <= 0) {
            continue;
        }
        if (waitpid(workers[i], &status, WNOHANG) == workers[i]) {
            worker_alive[i] = 0;
        }
    }
}

static void draw_lane(int id, int width, int position, long count, int alive) {
    static const char *colors[] = {
        "\033[1;32m", "\033[1;36m", "\033[1;35m", "\033[1;33m"
    };

    printf("worker %d pid=%ld %s events=%ld |",
           id, (long)workers[id], alive ? "alive" : "done ", count);
    for (int i = 0; i < width; ++i) {
        if (i == position) {
            printf("%s>\033[0m", colors[id % WORKER_COUNT]);
        } else if ((i + id) % 11 == 0) {
            printf("\033[0;2;37m.\033[0m");
        } else {
            putchar(' ');
        }
    }
    printf("|\n");
}

int main(void) {
    int pipe_fd[2];
    int read_flags;
    int lane_width;
    int lane_pos[WORKER_COUNT] = {0};
    long event_count[WORKER_COUNT] = {0};
    long total_events = 0;
    long empty_reads = 0;
    int frame = 0;

    if (stdout_is_unsafe_target()) {
        fprintf(stderr, "Refusing to run: stdout points to a file or block device.\n");
        return EXIT_FAILURE;
    }

    if (pipe(pipe_fd) != 0) {
        perror("pipe");
        return EXIT_FAILURE;
    }

    read_flags = fcntl(pipe_fd[0], F_GETFL);
    if (read_flags >= 0) {
        fcntl(pipe_fd[0], F_SETFL, read_flags | O_NONBLOCK);
    }

    signal(SIGINT, request_exit);
    signal(SIGTERM, request_exit);
#ifdef SIGHUP
    signal(SIGHUP, request_exit);
#endif
    atexit(restore_terminal);
    configure_keyboard();

    if (spawn_workers(pipe_fd[1]) != 0) {
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        perror("fork");
        return EXIT_FAILURE;
    }
    close(pipe_fd[1]);

    printf("\033[2J\033[?25l");
    while (!should_exit) {
        PipeEvent event;
        ssize_t count;

        poll_keyboard();

        do {
            count = read(pipe_fd[0], &event, sizeof(event));
            if (count == (ssize_t)sizeof(event)) {
                int cols = terminal_cols();

                lane_width = cols - 42;
                if (lane_width < MIN_LANE_WIDTH) {
                    lane_width = MIN_LANE_WIDTH;
                }
                if (lane_width > MAX_LANE_WIDTH) {
                    lane_width = MAX_LANE_WIDTH;
                }
                if (event.worker >= 0 && event.worker < WORKER_COUNT) {
                    lane_pos[event.worker] = event.tick % lane_width;
                    event_count[event.worker]++;
                    total_events++;
                }
            } else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                empty_reads++;
            }
        } while (count == (ssize_t)sizeof(event));

        update_worker_status();
        lane_width = terminal_cols() - 42;
        if (lane_width < MIN_LANE_WIDTH) {
            lane_width = MIN_LANE_WIDTH;
        }
        if (lane_width > MAX_LANE_WIDTH) {
            lane_width = MAX_LANE_WIDTH;
        }

        printf("\033[H");
        printf("\033[1;37mPipeflow: fork + pipe + O_NONBLOCK + waitpid(WNOHANG)\033[0m\n");
        printf("press q to exit | total_events=%ld empty_pipe_reads=%ld frame=%d\n",
               total_events, empty_reads, frame);
        for (int i = 0; i < WORKER_COUNT; ++i) {
            draw_lane(i, lane_width, lane_pos[i], event_count[i], worker_alive[i]);
        }
        printf("\033[J");
        fflush(stdout);

        frame++;
        sleep_ms(45);
    }

    close(pipe_fd[0]);
    stop_workers();
    return EXIT_SUCCESS;
}
