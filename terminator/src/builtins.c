/* builtins.c - PART 4: commands that must run INSIDE the shell */
#include "shell.h"
#include "history.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *names[] = { "cd", "pwd", "exit", "history", "hist-search", "jobs", "fg", "bg",
                               "export", "unset", "clear", "multiWatch", NULL };

int is_builtin(const char *name) {
    for (int i = 0; names[i]; i++) if (!strcmp(name, names[i])) return 1;
    return 0;
}

/* cd changes only THIS tab's cwd string. chdir() would change every tab in thread mode. */
static int bi_cd(shell_t *s, int argc, char **argv) {
    const char *target;
    if (argc > 2) { sh_err(s, "terminator: cd: too many arguments\n"); return 1; }
    if (argc == 1) {
        target = shell_get_var(s, "HOME");
        if (!target || !*target) { sh_err(s, "terminator: cd: HOME not set\n"); return 1; }
    } else if (!strcmp(argv[1], "-")) {
        target = shell_get_var(s, "OLDPWD");
        if (!target || !*target) { sh_err(s, "terminator: cd: OLDPWD not set\n"); return 1; }
        sh_out(s, "%s\n", target);
    } else target = argv[1];

    char joined[PATH_MAX * 2], resolved[PATH_MAX];
    if (target[0] == '/') snprintf(joined, sizeof joined, "%s", target);
    else snprintf(joined, sizeof joined, "%s/%s", s->cwd, target);
    if (!realpath(joined, resolved)) { sh_err(s, "terminator: cd: %s: %s\n", target, strerror(errno)); return 1; }
    struct stat st;
    if (stat(resolved, &st) < 0 || !S_ISDIR(st.st_mode)) { sh_err(s, "terminator: cd: %s: Not a directory\n", target); return 1; }
    if (access(resolved, X_OK) < 0) { sh_err(s, "terminator: cd: %s: %s\n", target, strerror(errno)); return 1; }
    shell_set_var(s, "OLDPWD", s->cwd);
    snprintf(s->cwd, sizeof s->cwd, "%s", resolved);
    shell_set_var(s, "PWD", s->cwd);
    return 0;
}

static int bi_exit(shell_t *s, int argc, char **argv) {
    s->exit_code = argc > 1 ? atoi(argv[1]) & 0xff : s->last_status;
    s->exit_requested = 1;
    return s->exit_code;
}

static int bi_fg(shell_t *s, int argc, char **argv) {
    job_t *j = job_find(s, argc > 1 ? argv[1] : NULL);
    if (!j) { sh_err(s, "terminator: fg: no such job\n"); return 1; }
    sh_out(s, "%s\n", j->cmd);
    return job_fg(s, j);
}

static int bi_bg(shell_t *s, int argc, char **argv) {
    job_t *j = job_find(s, argc > 1 ? argv[1] : NULL);
    if (!j) { sh_err(s, "terminator: bg: no such job\n"); return 1; }
    if (j->state != JOB_STOPPED) { sh_err(s, "terminator: bg: job %d already in background\n", j->id); return 1; }
    job_bg(s, j);
    return 0;
}

static int bi_export(shell_t *s, int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq) { char *nm = xstrndup(argv[i], (size_t)(eq - argv[i])); shell_set_var(s, nm, eq + 1); free(nm); }
    }   /* every shell variable is passed to children, so plain `export NAME` needs no work */
    return 0;
}

static int bi_unset(shell_t *s, int argc, char **argv) {
    for (int i = 1; i < argc; i++) shell_set_var(s, argv[i], NULL);
    return 0;
}

int run_builtin(shell_t *s, int argc, char **argv) {
    const char *n = argv[0];
    if (!strcmp(n, "cd")) return bi_cd(s, argc, argv);
    if (!strcmp(n, "pwd")) { sh_out(s, "%s\n", s->cwd); return 0; }
    if (!strcmp(n, "exit")) return bi_exit(s, argc, argv);
    if (!strcmp(n, "clear")) { sh_out(s, "\033[2J\033[H"); return 0; }
    if (!strcmp(n, "history")) { history_print(s->out_fd); return 0; }
    if (!strcmp(n, "hist-search")) {                     /* debug twin of Ctrl+R (Part 9 wires the key) */
        if (argc < 2) { sh_err(s, "usage: hist-search TERM\n"); return 1; }
        history_search(argv[1], s->out_fd); return 0;
    }
    if (!strcmp(n, "jobs")) { jobs_list(s); return 0; }
    if (!strcmp(n, "fg")) return bi_fg(s, argc, argv);
    if (!strcmp(n, "bg")) return bi_bg(s, argc, argv);
    if (!strcmp(n, "export")) return bi_export(s, argc, argv);
    if (!strcmp(n, "unset")) return bi_unset(s, argc, argv);
    if (!strcmp(n, "multiWatch")) return multiwatch_run(s, argc, argv);
    return 127;
}
