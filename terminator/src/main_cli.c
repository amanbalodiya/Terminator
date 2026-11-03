/* main_cli.c - PART 4 test harness: the shell engine in a plain terminal (no GUI).
 * Signals are delivered exactly the way the GUI will do it: our own handler calls
 * shell_signal_fg() -> kill(-pgid, sig). */
#include "shell.h"
#include "history.h"
#include "parser.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static shell_t *S;
static void on_int(int sig)  { (void)sig; shell_signal_fg(S, SIGINT);  write(1, "^C\n", 3); }
static void on_tstp(int sig) { (void)sig; shell_signal_fg(S, SIGTSTP); write(1, "^Z\n", 3); }

int main(void) {
    history_init(NULL);
    S = shell_new(0, -1, 1, 2);
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_int;  sigaction(SIGINT, &sa, NULL);
    sa.sa_handler = on_tstp; sigaction(SIGTSTP, &sa, NULL);

    char *acc = NULL, *line = NULL; size_t cap = 0;
    for (;;) {
        fputs(acc ? "> " : "user@terminator> ", stdout); fflush(stdout);
        ssize_t n = getline(&line, &cap, stdin);
        if (n < 0) { if (errno == EINTR) { clearerr(stdin); continue; } break; }
        if (n > 0 && line[n - 1] == '\n') line[n - 1] = 0;
        if (!acc) acc = strdup(line);
        else { acc = realloc(acc, strlen(acc) + strlen(line) + 2); strcat(acc, "\n"); strcat(acc, line); }
        if (line_incomplete(acc)) continue;                /* show "> " and keep reading */
        shell_submit(S, acc);
        free(acc); acc = NULL;
        if (S->exit_requested) break;
    }
    free(line); free(acc);
    int code = S->exit_code;
    shell_free(S);
    return code;
}
