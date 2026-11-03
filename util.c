/* util.c - PART 1 */
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

void *xmalloc(size_t n) { void *p = malloc(n ? n : 1); if (!p) { perror("malloc"); exit(1); } return p; }
void *xrealloc(void *p, size_t n) { p = realloc(p, n ? n : 1); if (!p) { perror("realloc"); exit(1); } return p; }
char *xstrdup(const char *s) { size_t n = strlen(s); char *p = xmalloc(n + 1); memcpy(p, s, n + 1); return p; }
char *xstrndup(const char *s, size_t n) { char *p = xmalloc(n + 1); memcpy(p, s, n); p[n] = 0; return p; }

void sl_init(strlist_t *l) { l->cap = 8; l->n = 0; l->v = xmalloc(l->cap * sizeof(char *)); l->v[0] = NULL; }
void sl_push(strlist_t *l, const char *s) {
    if (l->n + 2 > l->cap) { l->cap *= 2; l->v = xrealloc(l->v, l->cap * sizeof(char *)); }
    l->v[l->n++] = xstrdup(s);
    l->v[l->n] = NULL;
}
void sl_free(strlist_t *l) {
    for (int i = 0; i < l->n; i++) free(l->v[i]);
    free(l->v); l->v = NULL; l->n = l->cap = 0;
}

void buf_init(buf_t *b) { b->cap = 64; b->len = 0; b->s = xmalloc(b->cap); b->s[0] = 0; }
void buf_addn(buf_t *b, const char *d, size_t n) {
    if (b->len + n + 1 > b->cap) {
        while (b->len + n + 1 > b->cap) b->cap *= 2;
        b->s = xrealloc(b->s, b->cap);
    }
    memcpy(b->s + b->len, d, n); b->len += n; b->s[b->len] = 0;
}
void buf_addc(buf_t *b, char c) { buf_addn(b, &c, 1); }
void buf_adds(buf_t *b, const char *s) { buf_addn(b, s, strlen(s)); }
void buf_printf(buf_t *b, const char *fmt, ...) {
    va_list ap, ap2; va_start(ap, fmt); va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap); va_end(ap);
    if (n > 0) { char *t = xmalloc((size_t)n + 1); vsnprintf(t, (size_t)n + 1, fmt, ap2); buf_addn(b, t, (size_t)n); free(t); }
    va_end(ap2);
}
char *buf_take(buf_t *b) { char *s = b->s; b->s = NULL; b->len = b->cap = 0; return s; }
void buf_free(buf_t *b) { free(b->s); b->s = NULL; b->len = b->cap = 0; }

int write_all(int fd, const char *data, size_t n) {
    while (n > 0) {
        ssize_t w = write(fd, data, n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        data += w; n -= (size_t)w;
    }
    return 0;
}
