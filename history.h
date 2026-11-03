/* history.h - shared, locked, file-backed history. PART 5
 * One implementation serves BOTH modes: the table lives in an anonymous
 * MAP_SHARED mapping guarded by a process-shared semaphore, so threads and
 * forked tab processes see the same data. */
#ifndef HISTORY_H
#define HISTORY_H
#include <stddef.h>
#define HIST_MAX   10000
#define HIST_LINE  1024
#define HIST_SHOW  1000

int  history_init(const char *file);              /* NULL => ~/.terminator_history */
void history_add(const char *line);
int  history_get_back(int n_back, char *out, size_t outsz);  /* 0 = most recent; -1 if none */
unsigned long history_count(void);
void history_print(int fd);                       /* `history` builtin */
void history_search(const char *term, int fd);    /* Ctrl+R logic */
#endif
