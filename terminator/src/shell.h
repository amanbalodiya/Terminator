/* shell.h - per-tab shell session + public engine API. PART 4 */
#ifndef SHELL_H
#define SHELL_H
#include "common.h"
#include <limits.h>
#include <sys/types.h>

typedef struct { char *name; char *value; } var_t;

typedef enum { JOB_RUNNING, JOB_STOPPED, JOB_DONE } jobstate_t;
typedef struct {
    int used, id;
    pid_t pgid;
    int npids; pid_t *pids; int *pid_done; int live;
    int state, status, notified;
    char cmd[256];
} job_t;
#define MAX_JOBS 256

struct shell {
    int id;                              /* tab id */
    char cwd[PATH_MAX];                  /* PER-TAB working directory (never chdir() the process!) */
    int in_fd, out_fd, err_fd;           /* where children's stdin/stdout/stderr come from/go to */
    int last_status;                     /* $? */
    int exit_requested, exit_code;
    var_t *vars; int nvars, varcap;
    char *pos[10]; int nargs;            /* $0..$9, $# */
    job_t jobs[MAX_JOBS];
    volatile pid_t fg_pgid;              /* read by GUI thread to deliver Ctrl+C / Ctrl+Z */
    int depth;                           /* recursion guard for $(...) */
    const char *cur_stmt;                /* raw text of statement being run (used by multiWatch) */
};

/* session lifecycle */
shell_t *shell_new(int id, int in_fd, int out_fd, int err_fd);
void     shell_free(shell_t *s);

/* run text (may contain several statements); no history. returns last status */
int  shell_run_line(shell_t *s, const char *line);
/* what the GUI/CLI calls when the user presses Enter: history + run + reap jobs */
int  shell_submit(shell_t *s, const char *line);
/* run `cmd` capturing stdout (for $(...)); caller frees result */
char *shell_capture(shell_t *s, const char *cmd);

/* variables */
const char *shell_get_var(shell_t *s, const char *name);   /* shell vars, then environ */
void        shell_set_var(shell_t *s, const char *name, const char *value);

/* messages to the tab */
void sh_out(shell_t *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void sh_err(shell_t *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* jobs.c */
job_t *job_add(shell_t *s, pid_t pgid, const pid_t *pids, int n, const char *cmd, int state);
void   job_remove(shell_t *s, job_t *j);
int    job_wait_fg(shell_t *s, job_t *j);        /* waits; may leave job STOPPED */
int    job_fg(shell_t *s, job_t *j);             /* SIGCONT + wait (builtin fg) */
void   job_bg(shell_t *s, job_t *j);             /* SIGCONT, keep running */
job_t *job_find(shell_t *s, const char *spec);   /* "%n", "n", NULL => most recent */
void   jobs_reap(shell_t *s);                    /* collect finished bg jobs, print Done */
void   jobs_list(shell_t *s);
int    shell_signal_fg(shell_t *s, int sig);     /* GUI: kill(-fg_pgid, sig); 0 if nothing running */
void   job_print_stopped(shell_t *s, job_t *j);

/* builtins.c */
int is_builtin(const char *name);
int run_builtin(shell_t *s, int argc, char **argv);

/* multiWatch implementation */
int multiwatch_run(shell_t *s, int argc, char **argv);
#endif
