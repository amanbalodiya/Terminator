/* jobs.c - PART 3: job table, waiting, fg/bg, signal delivery for the GUI */
#include "shell.h"
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

static int lowest_free_id(shell_t *s) {
    for (int id = 1;; id++) {
        int used = 0;
        for (int i = 0; i < MAX_JOBS; i++) if (s->jobs[i].used && s->jobs[i].id == id) used = 1;
        if (!used) return id;
    }
}

job_t *job_add(shell_t *s, pid_t pgid, const pid_t *pids, int n, const char *cmd, int state) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (s->jobs[i].used) continue;
        job_t *j = &s->jobs[i];
        memset(j, 0, sizeof *j);
        j->used = 1; j->id = lowest_free_id(s); j->pgid = pgid; j->npids = n; j->live = n;
        j->pids = xmalloc((size_t)n * sizeof(pid_t)); memcpy(j->pids, pids, (size_t)n * sizeof(pid_t));
        j->pid_done = calloc((size_t)n, sizeof(int));
        j->state = state;
        snprintf(j->cmd, sizeof j->cmd, "%s", cmd);
        return j;
    }
    return NULL;
}

void job_remove(shell_t *s, job_t *j) { (void)s; free(j->pids); free(j->pid_done); memset(j, 0, sizeof *j); }

static int decode(int st) { return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st); }

/* marker: '+' most recent job, '-' the one before, ' ' others */
static char marker(shell_t *s, job_t *j) {
    int m1 = 0, m2 = 0;
    for (int i = 0; i < MAX_JOBS; i++) if (s->jobs[i].used) {
        int id = s->jobs[i].id;
        if (id > m1) { m2 = m1; m1 = id; } else if (id > m2) m2 = id;
    }
    return j->id == m1 ? '+' : j->id == m2 ? '-' : ' ';
}

void job_print_stopped(shell_t *s, job_t *j) { sh_out(s, "[%d]%c Stopped %s\n", j->id, marker(s, j), j->cmd); j->notified = 1; }

int job_wait_fg(shell_t *s, job_t *j) {
    (void)s;
    int stopped = 0;
    for (int i = 0; i < j->npids; i++) {
        if (j->pid_done[i]) continue;
        int st; pid_t r;
        do r = waitpid(j->pids[i], &st, WUNTRACED); while (r < 0 && errno == EINTR);
        if (r < 0) { j->pid_done[i] = 1; j->live--; continue; }
        if (WIFSTOPPED(st)) { stopped = 1; continue; }
        j->pid_done[i] = 1; j->live--;
        if (i == j->npids - 1) j->status = decode(st);
    }
    if (j->live == 0) j->state = JOB_DONE;
    else if (stopped) { j->state = JOB_STOPPED; j->status = 128 + SIGTSTP; }
    else j->state = JOB_RUNNING;
    return j->status;
}

int job_fg(shell_t *s, job_t *j) {
    j->state = JOB_RUNNING;
    s->fg_pgid = j->pgid;
    kill(-j->pgid, SIGCONT);
    int st = job_wait_fg(s, j);
    s->fg_pgid = 0;
    if (j->state == JOB_STOPPED) { job_print_stopped(s, j); return 128 + SIGTSTP; }
    job_remove(s, j);
    return st;
}

void job_bg(shell_t *s, job_t *j) {
    kill(-j->pgid, SIGCONT);
    j->state = JOB_RUNNING; j->notified = 0;
    sh_out(s, "[%d]%c %s &\n", j->id, marker(s, j), j->cmd);
}

job_t *job_find(shell_t *s, const char *spec) {
    if (spec && *spec == '%') spec++;
    if (spec && (*spec == 0 || !strcmp(spec, "+") || !strcmp(spec, "%"))) spec = NULL;
    job_t *best = NULL;
    for (int i = 0; i < MAX_JOBS; i++) {
        job_t *j = &s->jobs[i];
        if (!j->used) continue;
        if (spec) { if (j->id == atoi(spec)) return j; }
        else if (!best || j->id > best->id) best = j;
    }
    return spec ? NULL : best;
}

void jobs_reap(shell_t *s) {
    for (int i = 0; i < MAX_JOBS; i++) {
        job_t *j = &s->jobs[i];
        if (!j->used) continue;
        for (int k = 0; k < j->npids; k++) {
            if (j->pid_done[k]) continue;
            int st; pid_t r = waitpid(j->pids[k], &st, WNOHANG | WUNTRACED);
            if (r == j->pids[k]) {
                if (WIFSTOPPED(st)) { if (j->state != JOB_STOPPED) { j->state = JOB_STOPPED; j->notified = 0; } }
                else { j->pid_done[k] = 1; j->live--; if (k == j->npids - 1) j->status = decode(st); }
            } else if (r < 0 && errno != EINTR) { j->pid_done[k] = 1; j->live--; }
        }
        if (j->live == 0) { sh_out(s, "[%d]%c Done %s\n", j->id, marker(s, j), j->cmd); job_remove(s, j); }
        else if (j->state == JOB_STOPPED && !j->notified) { job_print_stopped(s, j); }
    }
}

void jobs_list(shell_t *s) {
    jobs_reap(s);
    for (int id = 1, left = 1; left; id++) {
        left = 0;
        for (int i = 0; i < MAX_JOBS; i++) {
            job_t *j = &s->jobs[i];
            if (!j->used) continue;
            if (j->id > id) left = 1;
            if (j->id != id) continue;
            if (j->state == JOB_STOPPED) sh_out(s, "[%d]%c Stopped %s\n", j->id, marker(s, j), j->cmd);
            else sh_out(s, "[%d]%c Running %s &\n", j->id, marker(s, j), j->cmd);
        }
    }
}

/* Called from the GUI event loop (another thread / process-side handler). Only THIS tab's
 * foreground group is signalled, never Terminator itself and never other tabs. */
int shell_signal_fg(shell_t *s, int sig) {
    pid_t g = s->fg_pgid;
    if (g <= 1) return 0;
    return kill(-g, sig) == 0;
}
