# 用法示例：
#   make
#   make tty_probe
#   make clean
#
# 说明：
#   F7LY rootfs 里如果没有 make，也可以直接使用 README 中的 cc 命令单独编译。

CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra

PROGRAMS := cyber starfield tty_probe pipeflow signal_scope hello_rust

.PHONY: all clean

all: $(PROGRAMS)

cyber: cyber.c
	$(CC) $(CFLAGS) -o $@ $<

starfield: starfield.c
	$(CC) $(CFLAGS) -o $@ $<

tty_probe: tty_probe.c
	$(CC) $(CFLAGS) -o $@ $<

pipeflow: pipeflow.c
	$(CC) $(CFLAGS) -o $@ $<

signal_scope: signal_scope.c
	$(CC) $(CFLAGS) -o $@ $<

hello_rust: hello_rust.rs
	rustc -o $@ $<

clean:
	rm -f $(PROGRAMS) a.out
	rm -f *.swp
	rm -f *.o