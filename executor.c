/* executor.c - PART 3: fork + execvp + pipes + redirection + process groups */
#include "executor.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct { int in, out, err; } rfd_t;

/* open relative to the TAB's cwd without touching the process-wide cwd (thread mode!) */
static int open_at_cwd(shell_t *s, const char *path, int flags, mode_t mode) {
    int d = open(s->cwd, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (d < 0) return -1;
    int fd = openat(d, path, flags | O_CLOEXEC, mode);
    int e = errno; close(d); errno = e;
    return fd;
}

static void close_rfd(rfd_t *r) {
    if (r->in >= 0) close(r->in);
    if (r->out >= 0) close(r->out);
    if (r->err >= 0) close(r->err);
    r->in = r->out = r->err = -1;
}

/* Open every redirection in the PARENT first, so a missing input file is reported and
 * the command is never started. Each open() gives a private file offset, so two
 * background jobs reading the same file cannot disturb each other. */
static int open_redirs(shell_t *s, cmd_t *c, rfd_t *r) {
    r->in = r->out = r->err = -1;
    if (c->in_file) {
        r->in = open_at_cwd(s, c->in_file, O_RDONLY, 0);
        if (r->in < 0) { sh_err(s, "terminator: %s: %s\n", c->in_file, strerror(errno)); return -1; }
    }
    if (c->out_file) {
        r->out = open_at_cwd(s, c->out_file, O_WRONLY | O_CREAT | (c->append ? O_APPEND : O_TRUNC), 0644);
        if (r->out < 0) { sh_err(s, "terminator: %s: %s\n", c->out_file, strerror(errno)); close_rfd(r); return -1; }
    }
    if (c->err_file) {
        r->err = open_at_cwd(s, c->err_file, O_WRONLY | O_CREAT | (c->err_append ? O_APPEND : O_TRUNC), 0644);
        if (r->err < 0) { sh_err(s, "terminator: %s: %s\n", c->err_file, strerror(errno)); close_rfd(r); return -1; }
    }
    return 0;
}

static int is_assignment(const char *w) {
    if (!(isalpha((unsigned char)w[0]) || w[0] == '_')) return 0;
    const char *p = w + 1;
    while (isalnum((unsigned char)*p) || *p == '_') p++;
    return *p == '=';
}

static void child_exec(shell_t *s, cmd_t *c, int in_fd, int out_fd, int err_fd, int n_pipes_fds[][2], int npipes) {
    /* default signal dispositions: the shell ignores some of these, children must not */
    signal(SIGINT, SIG_DFL); signal(SIGQUIT, SIG_DFL); signal(SIGTSTP, SIG_DFL);
    signal(SIGTTIN, SIG_DFL); signal(SIGTTOU, SIG_DFL); signal(SIGPIPE, SIG_DFL); signal(SIGCHLD, SIG_DFL);
    sigset_t none; sigemptyset(&none); sigprocmask(SIG_SETMASK, &none, NULL);

    if (in_fd  != 0) dup2(in_fd, 0);
    if (out_fd != 1) dup2(out_fd, 1);
    if (err_fd != 2) dup2(err_fd, 2);
    if (c->err_to_out) dup2(1, 2);
    for (int i = 0; i < npipes; i++) { close(n_pipes_fds[i][0]); close(n_pipes_fds[i][1]); }

    if (chdir(s->cwd) < 0) { dprintf(2, "terminator: %s: %s\n", s->cwd, strerror(errno)); _exit(1); }
    for (int i = 0; i < s->nvars; i++) setenv(s->vars[i].name, s->vars[i].value, 1);   /* child only */

    char *name = c->argv.v[0];
    if (is_builtin(name)) {
        s->out_fd = 1; s->err_fd = 2;
        _exit(run_builtin(s, c->argv.n, c->argv.v) & 0xff);
    }
    execvp(name, c->argv.v);
    int e = errno;
    if (e == ENOENT && !strchr(name, '/')) dprintf(2, "terminator: %s: command not found\n", name);
    else dprintf(2, "terminator: %s: %s\n", name, strerror(e));
    _exit(e == ENOENT ? 127 : 126);
}

int exec_pipeline(shell_t *s, pipeline_t *pl, const char *text) {
    int n = pl->n;
    if (n == 0) return s->last_status;

    rfd_t *rf = xmalloc((size_t)n * sizeof *rf);
    for (int i = 0; i < n; i++) {
        if (open_redirs(s, &pl->cmds[i], &rf[i]) < 0) {
            for (int k = 0; k < i; k++) close_rfd(&rf[k]);
            free(rf); return 1;
        }
    }

    int status = 0;
    cmd_t *c0 = &pl->cmds[0];

    /* ---- things that run inside the shell itself ---- */
    if (n == 1) {
        if (c0->argv.n == 0) { close_rfd(&rf[0]); free(rf); return 0; }      /* "> file" alone */

        int all_assign = 1;
        for (int i = 0; i < c0->argv.n; i++) if (!is_assignment(c0->argv.v[i])) all_assign = 0;
        if (all_assign) {
            for (int i = 0; i < c0->argv.n; i++) {
                char *eq = strchr(c0->argv.v[i], '=');
                char *nm = xstrndup(c0->argv.v[i], (size_t)(eq - c0->argv.v[i]));
                shell_set_var(s, nm, eq + 1); free(nm);
            }
            close_rfd(&rf[0]); free(rf); return 0;
        }
        if (!pl->background && is_builtin(c0->argv.v[0])) {
            int so = s->out_fd, se = s->err_fd;
            if (rf[0].out >= 0) s->out_fd = rf[0].out;
            if (rf[0].err >= 0) s->err_fd = rf[0].err;
            else if (c0->err_to_out) s->err_fd = s->out_fd;
            s->cur_stmt = text;
            status = run_builtin(s, c0->argv.n, c0->argv.v);
            s->out_fd = so; s->err_fd = se; s->cur_stmt = NULL;
            close_rfd(&rf[0]); free(rf); return status;
        }
    }

    /* ---- external pipeline: N children, N-1 pipes ---- */
    int (*pp)[2] = n > 1 ? xmalloc((size_t)(n - 1) * sizeof *pp) : NULL;
    for (int i = 0; i < n - 1; i++)
        if (pipe2(pp[i], O_CLOEXEC) < 0) {
            sh_err(s, "terminator: pipe: %s\n", strerror(errno));
            for (int k = 0; k < i; k++) { close(pp[k][0]); close(pp[k][1]); }
            for (int k = 0; k < n; k++) close_rfd(&rf[k]);
            free(pp); free(rf); return 1;
        }

    int devnull = open("/dev/null", O_RDONLY | O_CLOEXEC);
    pid_t *pids = xmalloc((size_t)n * sizeof *pids);
    pid_t pgid = 0; int started = 0;

    for (int i = 0; i < n; i++) {
        cmd_t *c = &pl->cmds[i];
        int in_fd, out_fd, err_fd;
        if (i > 0) in_fd = pp[i - 1][0];
        else if (rf[i].in >= 0) in_fd = rf[i].in;
        else in_fd = (!pl->background && s->in_fd >= 0) ? s->in_fd : devnull;
        if (rf[i].in >= 0 && i > 0) in_fd = rf[i].in;                        /* explicit < wins */
        if (i < n - 1) out_fd = pp[i][1]; else out_fd = s->out_fd;
        if (rf[i].out >= 0) out_fd = rf[i].out;
        err_fd = rf[i].err >= 0 ? rf[i].err : s->err_fd;

        pid_t pid = fork();
        if (pid < 0) { sh_err(s, "terminator: fork: %s\n", strerror(errno)); status = 1; break; }
        if (pid == 0) {
            setpgid(0, pgid ? pgid : 0);                                      /* one group per pipeline */
            child_exec(s, c, in_fd, out_fd, err_fd, pp, n - 1);
        }
        if (pgid == 0) pgid = pid;
        setpgid(pid, pgid);                                                   /* both sides: avoids a race */
        pids[started++] = pid;
    }

    for (int i = 0; i < n - 1; i++) { close(pp[i][0]); close(pp[i][1]); }     /* a forgotten write end = no EOF */
    for (int i = 0; i < n; i++) close_rfd(&rf[i]);
    if (devnull >= 0) close(devnull);
    free(pp); free(rf);

    if (started == 0) { free(pids); return status ? status : 1; }

    char label[256]; snprintf(label, sizeof label, "%s", text);
    for (char *q = label + strlen(label); q > label && isspace((unsigned char)q[-1]); ) *--q = 0;
    const char *lp = label; while (isspace((unsigned char)*lp)) lp++;

    job_t *j = job_add(s, pgid, pids, started, lp, JOB_RUNNING);
    if (!j) { sh_err(s, "terminator: too many jobs\n"); kill(-pgid, SIGKILL); free(pids); return 1; }
    free(pids);

    if (pl->background) {
        sh_out(s, "[%d] %d\n", j->id, (int)pgid);
        return 0;
    }

    s->fg_pgid = pgid;
    status = job_wait_fg(s, j);
    s->fg_pgid = 0;
    if (j->state == JOB_STOPPED) { job_print_stopped(s, j); return 128 + SIGTSTP; }
    job_remove(s, j);
    return status;
}
