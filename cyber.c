#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>

#define MAX_COLS 300

void sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

int main() {
    struct winsize w;
    int rows = 24;
    int cols = 80;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0) {
        rows = w.ws_row;
        cols = w.ws_col;
    }

    if (cols > MAX_COLS) {
        cols = MAX_COLS;
    }

    int drops[MAX_COLS];

    srand(time(NULL));

    for (int i = 0; i < cols; i++) {
        drops[i] = rand() % rows;
    }

    printf("\033[2J");      // 清屏
    printf("\033[?25l");    // 隐藏光标

    while (1) {
        printf("\033[H");   // 光标回到左上角

        for (int y = 0; y < rows; y++) {
            for (int x = 0; x < cols; x++) {
                if (drops[x] == y) {
                    char c = 33 + rand() % 94;
                    printf("\033[1;32m%c\033[0m", c);
                } else if (drops[x] - 1 == y) {
                    char c = 33 + rand() % 94;
                    printf("\033[0;32m%c\033[0m", c);
                } else if (drops[x] - 2 == y) {
                    char c = 33 + rand() % 94;
                    printf("\033[0;2;32m%c\033[0m", c);
                } else {
                    printf(" ");
                }
            }
            printf("\n");
        }

        for (int i = 0; i < cols; i++) {
            drops[i]++;

            if (drops[i] > rows + rand() % 20) {
                drops[i] = 0;
            }
        }

        fflush(stdout);
        sleep_ms(60);
    }

    printf("\033[?25h");    // 恢复光标
    return 0;
}