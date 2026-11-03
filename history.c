/* history.c - PART 5 */
#include "history.h"
#include "common.h"
#include <semaphore.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>

typedef struct {
    sem_t lock;
    unsigned long total;                 /* commands ever added (monotonic) */
    char path[PATH_MAX];
    char lines[HIST_MAX][HIST_LINE];     /* ring buffer: entry i lives at i % HIST_MAX */
} hist_t;

static hist_t *H;

static void lock(void)   { while (sem_wait(&H->lock) < 0 && errno == EINTR) ; }
static void unlock(void) { sem_post(&H->lock); }

static void normalize(const char *in, char *out) {
    size_t n = 0;
    for (; *in && n < HIST_LINE - 1; in++) out[n++] = (*in == '\n' || *in == '\r') ? ' ' : *in;
    while (n > 0 && isspace((unsigned char)out[n - 1])) n--;
    out[n] = 0;
}

static void push_locked(const char *line) {
    strcpy(H->lines[H->total % HIST_MAX], line);
    H->total++;
}

int history_init(const char *file) {
    if (H) return 0;
    H = mmap(NULL, sizeof(hist_t), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (H == MAP_FAILED) { H = NULL; return -1; }
    sem_init(&H->lock, 1, 1);                       /* pshared = 1 */
    if (file) snprintf(H->path, sizeof H->path, "%s", file);
    else { const char *h = getenv("HOME"); snprintf(H->path, sizeof H->path, "%s/.terminator_history", h ? h : "."); }

    FILE *f = fopen(H->path, "r");
    if (!f) return 0;
    char *ln = NULL; size_t cap = 0; ssize_t r; unsigned long lines_read = 0;
    char tmp[HIST_LINE];
    while ((r = getline(&ln, &cap, f)) >= 0) {
        normalize(ln, tmp);
        if (tmp[0]) { push_locked(tmp); lines_read++; }
    }
    free(ln); fclose(f);
    if (lines_read > HIST_MAX) {                     /* trim file to the last HIST_MAX */
        f = fopen(H->path, "w");
        if (f) {
            unsigned long start = H->total > HIST_MAX ? H->total - HIST_MAX : 0;
            for (unsigned long i = start; i < H->total; i++) fprintf(f, "%s\n", H->lines[i % HIST_MAX]);
            fclose(f);
        }
    }
    return 0;
}

void history_add(const char *line) {
    if (!H) return;
    char tmp[HIST_LINE]; normalize(line, tmp);
    if (!tmp[0]) return;
    lock();
    if (H->total > 0 && strcmp(H->lines[(H->total - 1) % HIST_MAX], tmp) == 0) { unlock(); return; }
    push_locked(tmp);
    int fd = open(H->path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd >= 0) { char *o = xmalloc(strlen(tmp) + 2); sprintf(o, "%s\n", tmp); write_all(fd, o, strlen(o)); free(o); close(fd); }
    unlock();
}

unsigned long history_count(void) {
    if (!H) return 0;
    lock(); unsigned long c = H->total > HIST_MAX ? HIST_MAX : H->total; unlock();
    return c;
}

int history_get_back(int n_back, char *out, size_t outsz) {
    if (!H || n_back < 0) return -1;
    lock();
    unsigned long avail = H->total > HIST_MAX ? HIST_MAX : H->total;
    if ((unsigned long)n_back >= avail) { unlock(); return -1; }
    snprintf(out, outsz, "%s", H->lines[(H->total - 1 - (unsigned long)n_back) % HIST_MAX]);
    unlock();
    return 0;
}

void history_print(int fd) {
    if (!H) return;
    buf_t b; buf_init(&b);
    lock();
    unsigned long avail = H->total > HIST_MAX ? HIST_MAX : H->total;
    unsigned long cnt = avail > HIST_SHOW ? HIST_SHOW : avail;
    for (unsigned long i = H->total - cnt; i < H->total; i++)
        buf_printf(&b, "%5lu  %s\n", i + 1, H->lines[i % HIST_MAX]);
    unlock();
    write_all(fd, b.s, b.len);          /* write AFTER unlocking: fd may block */
    buf_free(&b);
}

/* longest common SUBSTRING length of a[0..n) and b[0..m) */
static size_t lcs_len(const char *a, size_t n, const char *b, size_t m, size_t *prev, size_t *cur) {
    size_t best = 0;
    memset(prev, 0, (m + 1) * sizeof(size_t));
    for (size_t i = 1; i <= n; i++) {
        cur[0] = 0;
        for (size_t j = 1; j <= m; j++) {
            if (a[i - 1] == b[j - 1]) { cur[j] = prev[j - 1] + 1; if (cur[j] > best) best = cur[j]; }
            else cur[j] = 0;
        }
        size_t *t = prev; prev = cur; cur = t;
    }
    return best;
}

void history_search(const char *term_in, int fd) {
    static const char *nomatch = "No match for search term in history\n";
    if (!H) return;
    char term[HIST_LINE]; normalize(term_in, term);
    size_t m = strlen(term);
    if (m == 0) { write_all(fd, nomatch, strlen(nomatch)); return; }
    buf_t b; buf_init(&b);
    lock();
    unsigned long avail = H->total > HIST_MAX ? HIST_MAX : H->total;
    int found = 0;
    /* (a) most recent exact match */
    for (unsigned long k = 0; k < avail && !found; k++) {
        const char *e = H->lines[(H->total - 1 - k) % HIST_MAX];
        if (strcmp(e, term) == 0) { buf_printf(&b, "%s\n", e); found = 1; }
    }
    /* (b) longest common substring, must be > 2 chars; print every entry that ties for best */
    if (!found) {
        size_t *p1 = xmalloc((m + 1) * sizeof(size_t)), *p2 = xmalloc((m + 1) * sizeof(size_t));
        size_t best = 0;
        for (unsigned long k = 0; k < avail; k++) {
            const char *e = H->lines[(H->total - 1 - k) % HIST_MAX];
            size_t l = lcs_len(e, strlen(e), term, m, p1, p2);
            if (l > best) best = l;
        }
        if (best > 2) {
            const char *seen[64]; int nseen = 0;
            for (unsigned long k = 0; k < avail; k++) {          /* most recent first */
                const char *e = H->lines[(H->total - 1 - k) % HIST_MAX];
                if (lcs_len(e, strlen(e), term, m, p1, p2) != best) continue;
                int dup = 0;
                for (int i = 0; i < nseen; i++) if (!strcmp(seen[i], e)) dup = 1;
                if (dup) continue;
                buf_printf(&b, "%s\n", e);
                if (nseen < 64) seen[nseen++] = e;
                found = 1;
            }
        }
        free(p1); free(p2);
    }
    unlock();
    if (!found) write_all(fd, nomatch, strlen(nomatch));
    else write_all(fd, b.s, b.len);
    buf_free(&b);
}
