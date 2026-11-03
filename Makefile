# Terminator - Makefile
CC      ?= gcc
CFLAGS  ?= -std=gnu11 -Wall -Wextra -Wno-unused-parameter -g -O0 -pthread -D_GNU_SOURCE -Isrc $(shell pkg-config --cflags xft)
LDLIBS  := -pthread

CORE := src/util.c src/history.c src/parser.c src/executor.c src/jobs.c \
        src/builtins.c src/shell.c src/multiwatch.c

# GUI-side files appear in later parts; build them only if they exist.
GUI  := src/main.c

ifneq ($(wildcard src/main.c),)
all: terminator shell_test
else
all: shell_test
endif

# final product (needs libx11-dev)
terminator: $(CORE) $(GUI)
	$(CC) $(CFLAGS) -o $@ $^ $(shell pkg-config --libs x11 xft) $(LDLIBS)

# GUI-less engine for testing Parts 1-5 in a normal terminal
shell_test: $(CORE) src/main_cli.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

clean:
	rm -f terminator shell_test

.PHONY: all clean
