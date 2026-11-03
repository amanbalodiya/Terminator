/* shell.c - PART 4: session state, variables, statement loop, command substitution */
#include "shell.h"
#include "executor.h"
#include "history.h"
#include "parser.h"
#include <ctype.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void sh_vprint(shell_t *s, int fd, const char *fmt, va_list ap) {
    if (fd < 0) return;
    va_list ap2; va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    if (n > 0) {
        char *t = xmalloc((size_t)n + 1);
        vsnprintf(t, (size_t)n + 1, fmt, ap2);
        write_all(fd, t, (size_t)n);
        free(t);
    }
    va_end(ap2); (void)s;
}
void sh_out(shell_t *s, const char *fmt, ...) { va_list ap; va_start(ap, fmt); sh_vprint(s, s->out_fd, fmt, ap); va_end(ap); }
void sh_err(shell_t *s, const char *fmt, ...) { va_list ap; va_start(ap, fmt); sh_vprint(s, s->err_fd, fmt, ap); va_end(ap); }

shell_t *shell_new(int id, int in_fd, int out_fd, int err_fd) {
    static int once;
    if (!once) { once = 1; signal(SIGPIPE, SIG_IGN); }       /* a dead reader must not kill Terminator */
    shell_t *s = calloc(1, sizeof *s);
    s->id = id; s->in_fd = in_fd; s->out_fd = out_fd; s->err_fd = err_fd;
    if (!getcwd(s->cwd, sizeof s->cwd)) strcpy(s->cwd, "/");
    s->pos[0] = xstrdup("terminator");
    return s;
}

void shell_free(shell_t *s) {
    for (int i = 0; i < MAX_JOBS; i++) if (s->jobs[i].used) {
        kill(-s->jobs[i].pgid, SIGHUP); kill(-s->jobs[i].pgid, SIGCONT);
        job_remove(s, &s->jobs[i]);
    }
    for (int i = 0; i < s->nvars; i++) { free(s->vars[i].name); free(s->vars[i].value); }
    free(s->vars);
    for (int i = 0; i < 10; i++) free(s->pos[i]);
    free(s);
}

const char *shell_get_var(shell_t *s, const char *name) {
    for (int i = 0; i < s->nvars; i++) if (!strcmp(s->vars[i].name, name)) return s->vars[i].value;
    return getenv(name);
}

void shell_set_var(shell_t *s, const char *name, const char *value) {
    for (int i = 0; i < s->nvars; i++) if (!strcmp(s->vars[i].name, name)) {
        if (!value) {                                   /* unset */
            free(s->vars[i].name); free(s->vars[i].value);
            s->vars[i] = s->vars[--s->nvars]; return;
        }
        free(s->vars[i].value); s->vars[i].value = xstrdup(value); return;
    }
    if (!value) return;
    if (s->nvars == s->varcap) { s->varcap = s->varcap ? s->varcap * 2 : 16; s->vars = xrealloc(s->vars, (size_t)s->varcap * sizeof(var_t)); }
    s->vars[s->nvars].name = xstrdup(name); s->vars[s->nvars].value = xstrdup(value); s->nvars++;
}

static int blank(const char *p) { while (*p) if (!isspace((unsigned char)*p++)) return 0; return 1; }

int shell_run_line(shell_t *s, const char *line) {
    if (s->depth > 32) { sh_err(s, "terminator: maximum nesting depth exceeded\n"); return 1; }
    s->depth++;
    const char *p = line; int prev = T_NONE;
    while (*p && !s->exit_requested) {
        int term; const char *end = next_statement_end(p, &term);
        char *stmt = xstrndup(p, (size_t)(end - p));
        int run = 1;
        if (prev == T_AND && s->last_status != 0) run = 0;       /* a && b */
        if (prev == T_OR  && s->last_status == 0) run = 0;       /* a || b */
        if (run && !blank(stmt)) {
            pipeline_t pl;
            if (parse_pipeline(s, stmt, &pl) == 0) {
                pl.background = (term == T_BG);
                s->last_status = exec_pipeline(s, &pl, stmt);
                pipeline_free(&pl);
            } else s->last_status = 2;
        }
        free(stmt);
        if (term == T_NONE) break;
        p = end + ((term == T_AND || term == T_OR) ? 2 : 1);
        prev = term;
    }
    s->depth--;
    return s->last_status;
}

int shell_submit(shell_t *s, const char *line) {
    if (strncmp(line, "hist-search", 11) != 0) history_add(line);   /* debug builtin: keep it out of the results */
    int st = shell_run_line(s, line);
    jobs_reap(s);
    return st;
}

/* $(...) : run in-process with stdout going to a temp file, then read it back.
 * (A temp file instead of a pipe avoids a deadlock when output exceeds the pipe size.)
 * Behaves like a subshell: cwd and `exit` do not leak out. */
char *shell_capture(shell_t *s, const char *cmd) {
    FILE *tmp = tmpfile();
    if (!tmp) return xstrdup("");
    int fd = fileno(tmp), save_out = s->out_fd, save_exit = s->exit_requested;
    char save_cwd[PATH_MAX]; snprintf(save_cwd, sizeof save_cwd, "%s", s->cwd);
    s->out_fd = fd; s->exit_requested = 0;
    shell_run_line(s, cmd);
    s->out_fd = save_out; s->exit_requested = save_exit;
    snprintf(s->cwd, sizeof s->cwd, "%s", save_cwd);

    buf_t b; buf_init(&b);
    lseek(fd, 0, SEEK_SET);
    char chunk[4096]; ssize_t r;
    while ((r = read(fd, chunk, sizeof chunk)) > 0) buf_addn(&b, chunk, (size_t)r);
    fclose(tmp);
    while (b.len > 0 && b.s[b.len - 1] == '\n') b.s[--b.len] = 0;     /* strip trailing newlines */
    return buf_take(&b);
}
