#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <termios.h>

#define STAR_COUNT 700

typedef struct {
    float x;
    float y;
    float z;
    float speed;
} Star;

int rows = 24;
int cols = 80;
Star stars[STAR_COUNT];

static struct termios original_termios;
static int termios_changed = 0;
static int original_stdin_flags = -1;
static int stdin_flags_changed = 0;
static int stdin_polling_enabled = 0;
static volatile sig_atomic_t should_exit = 0;
static int cleanup_finished = 0;

float frand_range(float a, float b) {
    return a + (float)rand() / RAND_MAX * (b - a);
}

static void request_exit(int signal_number) {
    (void)signal_number;
    should_exit = 1;
}

static void restore_terminal(void) {
    if (cleanup_finished) return;
    cleanup_finished = 1;

    if (termios_changed)
        tcsetattr(STDIN_FILENO, TCSANOW, &original_termios);
    if (stdin_flags_changed)
        fcntl(STDIN_FILENO, F_SETFL, original_stdin_flags);

    printf("\033[0m");
    printf("\033[?25h");
    printf("\033[2J");
    printf("\033[H");
    printf("Starfield stopped.\n");
    fflush(stdout);
}

void cleanup(int sig) {
    (void)sig;
    restore_terminal();
    exit(0);
}

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

static void check_quit_key(void) {
    char input[32];
    ssize_t count;

    if (!stdin_polling_enabled) return;

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

void sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

void get_terminal_size() {
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0) {
        rows = w.ws_row;
        cols = w.ws_col;
    }
    if (rows < 10) rows = 10;
    if (cols < 20) cols = 20;
}

void reset_star(int i) {
    stars[i].x = frand_range(-1.0f, 1.0f);
    stars[i].y = frand_range(-0.6f, 0.6f);
    stars[i].z = frand_range(0.2f, 1.0f);
    stars[i].speed = frand_range(0.006f, 0.030f);
}

int main() {
    int raw_input_enabled;

    signal(SIGINT, request_exit);
    signal(SIGTERM, request_exit);
#ifdef SIGHUP
    signal(SIGHUP, request_exit);
#endif
    atexit(restore_terminal);
    srand(time(NULL));

    get_terminal_size();

    for (int i = 0; i < STAR_COUNT; i++) {
        reset_star(i);
        stars[i].z = frand_range(0.2f, 1.0f);
    }

    char *screen = malloc(rows * cols);
    char *shade = malloc(rows * cols);

    if (!screen || !shade) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }

    printf("\033[2J");
    printf("\033[?25l");

    raw_input_enabled = configure_keyboard();

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
        if (should_exit) break;
        get_terminal_size();

        screen = realloc(screen, rows * cols);
        shade = realloc(shade, rows * cols);

        if (!screen || !shade) {
            cleanup(0);
        }

        for (int i = 0; i < rows * cols; i++) {
            screen[i] = ' ';
            shade[i] = 0;
        }

        for (int i = 0; i < STAR_COUNT; i++) {
            stars[i].z -= stars[i].speed;

            if (stars[i].z <= 0.02f) {
                reset_star(i);
            }

            int sx = cols / 2 + (int)(stars[i].x / stars[i].z * cols / 2);
            int sy = rows / 2 + (int)(stars[i].y / stars[i].z * rows / 2);

            if (sx < 1 || sx >= cols - 1 || sy < 1 || sy >= rows - 1) {
                reset_star(i);
                continue;
            }

            int pos = sy * cols + sx;
            float depth = 1.0f - stars[i].z;

            if (depth > 0.85f) {
                screen[pos] = '@';
                shade[pos] = 4;
            } else if (depth > 0.65f) {
                screen[pos] = 'O';
                shade[pos] = 3;
            } else if (depth > 0.45f) {
                screen[pos] = '*';
                shade[pos] = 2;
            } else if (depth > 0.25f) {
                screen[pos] = '+';
                shade[pos] = 1;
            } else {
                screen[pos] = '.';
                shade[pos] = 0;
            }

            if (shade[pos] >= 3 && sx + 1 < cols) {
                screen[pos + 1] = '-';
                shade[pos + 1] = 2;
            }
            if (shade[pos] >= 4 && sx - 1 >= 0) {
                screen[pos - 1] = '-';
                shade[pos - 1] = 2;
            }
        }

        printf("\033[H");

        /* 显示退出提示 */
        printf("\033[1;37m%-*.*s\033[0m\n", cols, cols, quit_prompt);

        for (int y = 0; y < rows; y++) {
            for (int x = 0; x < cols; x++) {
                int pos = y * cols + x;

                if (shade[pos] == 4) {
                    printf("\033[1;37m%c\033[0m", screen[pos]);
                } else if (shade[pos] == 3) {
                    printf("\033[1;36m%c\033[0m", screen[pos]);
                } else if (shade[pos] == 2) {
                    printf("\033[0;36m%c\033[0m", screen[pos]);
                } else if (shade[pos] == 1) {
                    printf("\033[0;34m%c\033[0m", screen[pos]);
                } else {
                    printf("\033[0;2;37m%c\033[0m", screen[pos]);
                }
            }
            putchar('\n');
        }

        fflush(stdout);
        sleep_ms(30);
    }

    return 0;
}
