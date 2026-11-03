/* common.h - tiny shared helpers (strings, buffers, safe alloc). PART 1 */
#ifndef COMMON_H
#define COMMON_H
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>

typedef struct shell shell_t;

typedef struct { char **v; int n; int cap; } strlist_t;      /* NULL-terminated */
typedef struct { char *s; size_t len; size_t cap; } buf_t;   /* growable string */

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

void sl_init(strlist_t *l);
void sl_push(strlist_t *l, const char *s);   /* duplicates s */
void sl_free(strlist_t *l);

void  buf_init(buf_t *b);
void  buf_addn(buf_t *b, const char *d, size_t n);
void  buf_addc(buf_t *b, char c);
void  buf_adds(buf_t *b, const char *s);
void  buf_printf(buf_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
char *buf_take(buf_t *b);                    /* caller frees; buf is reset */
void  buf_free(buf_t *b);

int write_all(int fd, const char *data, size_t n);  /* handles EINTR/partial */
#endif
