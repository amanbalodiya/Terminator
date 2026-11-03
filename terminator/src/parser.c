/* parser.c - PART 2 */
#include "parser.h"
#include "shell.h"
#include <ctype.h>
#include <glob.h>
#include <stdlib.h>
#include <string.h>

/* The spec's example shows  "...exciting?\<Enter>Fork..."  printing on TWO lines, so inside
 * double quotes a backslash-newline keeps the newline. (Real bash would join the lines;
 * flip this to 0 for bash behaviour.) Outside quotes, backslash-newline is always removed. */
#define DQ_BACKSLASH_NEWLINE_KEEPS_NEWLINE 1

/* ------------------------------------------------------------------ skippers */
const char *skip_single(const char *p) { while (*p && *p != '\'') p++; return *p ? p : NULL; }

const char *skip_backtick(const char *p) {
    while (*p) {
        if (*p == '\\' && p[1]) { p += 2; continue; }
        if (*p == '`') return p;
        p++;
    }
    return NULL;
}

const char *skip_double(const char *p) {
    const char *q;
    while (*p) {
        if (*p == '\\' && p[1]) { p += 2; continue; }
        if (*p == '"') return p;
        if (*p == '`') { q = skip_backtick(p + 1); if (!q) return NULL; p = q + 1; continue; }
        if (*p == '$' && p[1] == '(') { q = skip_dollar_paren(p + 2); if (!q) return NULL; p = q + 1; continue; }
        p++;
    }
    return NULL;
}

const char *skip_dollar_paren(const char *p) {
    int depth = 1; const char *q;
    while (*p) {
        switch (*p) {
        case '\\': if (p[1]) p++; break;
        case '\'': q = skip_single(p + 1); if (!q) return NULL; p = q; break;
        case '"':  q = skip_double(p + 1); if (!q) return NULL; p = q; break;
        case '`':  q = skip_backtick(p + 1); if (!q) return NULL; p = q; break;
        case '(':  depth++; break;
        case ')':  if (--depth == 0) return p; break;
        }
        p++;
    }
    return NULL;
}

static int at_word_start(const char *base, const char *p) {
    return p == base || isspace((unsigned char)p[-1]) || strchr(";&|(", p[-1]) != NULL;
}

/* -------------------------------------------------------- completeness check */
int line_incomplete(const char *s) {
    const char *p = s, *q;
    while (*p) {
        char c = *p;
        if (c == '\\') { if (!p[1]) return 1; p += 2; continue; }
        if (c == '\'') { q = skip_single(p + 1); if (!q) return 1; p = q + 1; continue; }
        if (c == '"')  { q = skip_double(p + 1); if (!q) return 1; p = q + 1; continue; }
        if (c == '`')  { q = skip_backtick(p + 1); if (!q) return 1; p = q + 1; continue; }
        if (c == '$' && p[1] == '(') { q = skip_dollar_paren(p + 2); if (!q) return 1; p = q + 1; continue; }
        if (c == '#' && at_word_start(s, p)) { while (*p && *p != '\n') p++; continue; }
        p++;
    }
    const char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) e--;
    if (e > s && e[-1] == '|') return 1;                       /* "a |" and "a ||" */
    if (e - s >= 2 && e[-2] == '&' && e[-1] == '&') return 1;
    return 0;
}

/* ---------------------------------------------------------- statement splitter */
const char *next_statement_end(const char *s, int *term) {
    const char *p = s, *q;
    while (*p) {
        char c = *p;
        if (c == '\\') { p += p[1] ? 2 : 1; continue; }
        if (c == '\'') { q = skip_single(p + 1); if (!q) break; p = q + 1; continue; }
        if (c == '"')  { q = skip_double(p + 1); if (!q) break; p = q + 1; continue; }
        if (c == '`')  { q = skip_backtick(p + 1); if (!q) break; p = q + 1; continue; }
        if (c == '$' && p[1] == '(') { q = skip_dollar_paren(p + 2); if (!q) break; p = q + 1; continue; }
        if (c == '#' && at_word_start(s, p)) { while (*p && *p != '\n') p++; continue; }
        if (c == ';' || c == '\n') { *term = T_SEMI; return p; }
        if (c == '&') {
            if (p[1] == '&') { *term = T_AND; return p; }
            if (p > s && p[-1] == '>') { p++; continue; }      /* part of 2>&1 */
            *term = T_BG; return p;
        }
        if (c == '|' && p[1] == '|') { *term = T_OR; return p; }
        p++;
    }
    *term = T_NONE;
    return p + strlen(p);
}

/* ------------------------------------------------------------------- lexer */
typedef struct {
    shell_t *s;
    cmd_t cur;
    int pending;                 /* 0 none; 1 '<'; 2 '>'; 3 '>>'; 4 '2>'; 5 '2>>' */
    buf_t val, pat;              /* literal text, and glob pattern (quoted chars escaped) */
    int active, has_glob;
} lex_t;

static void syntax(shell_t *s, const char *near) { sh_err(s, "terminator: syntax error near `%s'\n", near); }

static void lex_putc(lex_t *L, char c, int quoted) {
    L->active = 1;
    buf_addc(&L->val, c);
    if (quoted && strchr("*?[\\", c)) buf_addc(&L->pat, '\\');
    else if (!quoted && strchr("*?[", c)) L->has_glob = 1;
    buf_addc(&L->pat, c);
}

static void glob_expand(shell_t *s, const char *pat, const char *lit, strlist_t *out) {
    buf_t full; buf_init(&full);
    size_t strip = 0;
    if (pat[0] != '/') {                       /* glob() uses the process cwd; we need the TAB's cwd */
        const char *base = strcmp(s->cwd, "/") == 0 ? "" : s->cwd;
        for (const char *c = base; *c; c++) { if (strchr("*?[\\", *c)) buf_addc(&full, '\\'); buf_addc(&full, *c); }
        buf_addc(&full, '/');
        strip = strlen(base) + 1;
    }
    buf_adds(&full, pat);
    glob_t g; int rc = glob(full.s, 0, NULL, &g);
    if (rc == 0 && g.gl_pathc > 0) {
        for (size_t i = 0; i < g.gl_pathc; i++) {
            const char *r = g.gl_pathv[i];
            if (strlen(r) >= strip) r += strip;
            sl_push(out, r);
        }
    } else sl_push(out, lit);
    if (rc == 0) globfree(&g);
    buf_free(&full);
}

static void lex_flush(lex_t *L) {
    if (!L->active) return;
    strlist_t out; sl_init(&out);
    if (L->has_glob) glob_expand(L->s, L->pat.s, L->val.s, &out); else sl_push(&out, L->val.s);
    if (L->pending) {
        char **slot = NULL;
        switch (L->pending) {
        case 1: slot = &L->cur.in_file; break;
        case 2: slot = &L->cur.out_file; L->cur.append = 0; break;
        case 3: slot = &L->cur.out_file; L->cur.append = 1; break;
        case 4: slot = &L->cur.err_file; L->cur.err_append = 0; L->cur.err_to_out = 0; break;
        case 5: slot = &L->cur.err_file; L->cur.err_append = 1; L->cur.err_to_out = 0; break;
        }
        free(*slot); *slot = xstrdup(out.v[0]);
        L->pending = 0;
    } else for (int i = 0; i < out.n; i++) sl_push(&L->cur.argv, out.v[i]);
    sl_free(&out);
    L->val.len = 0; L->val.s[0] = 0; L->pat.len = 0; L->pat.s[0] = 0;
    L->active = 0; L->has_glob = 0;
}

/* put an expansion result into the current word. Unquoted => field splitting + globbing. */
static void emit(lex_t *L, const char *str, int quoted) {
    for (; *str; str++) {
        if (quoted) lex_putc(L, *str, 1);
        else if (isspace((unsigned char)*str)) lex_flush(L);
        else lex_putc(L, *str, 0);
    }
}

static void emit_capture(lex_t *L, const char *inner, int quoted) {
    char *out = shell_capture(L->s, inner);
    emit(L, out, quoted);
    free(out);
}

/* p points at '$'. Advances *pp past the construct. */
static void expand_dollar(lex_t *L, const char **pp, int quoted) {
    const char *p = *pp + 1, *val = NULL; char *owned = NULL; char name[256]; char num[32];
    if (*p == '(') {
        const char *end = skip_dollar_paren(p + 1);
        if (!end) { syntax(L->s, "$("); *pp += strlen(*pp); return; }
        char *inner = xstrndup(p + 1, (size_t)(end - p - 1));
        owned = shell_capture(L->s, inner); free(inner); val = owned; p = end + 1;
    } else if (*p == '{') {
        const char *end = strchr(p, '}');
        if (!end) { lex_putc(L, '$', quoted); *pp = p; return; }
        size_t n = (size_t)(end - p - 1); if (n >= sizeof name) n = sizeof name - 1;
        memcpy(name, p + 1, n); name[n] = 0;
        val = shell_get_var(L->s, name); p = end + 1;
    } else if (*p == '?') { snprintf(num, sizeof num, "%d", L->s->last_status); val = num; p++; }
    else if (*p == '#')   { snprintf(num, sizeof num, "%d", L->s->nargs); val = num; p++; }
    else if (isdigit((unsigned char)*p)) { int d = *p - '0'; val = (d <= L->s->nargs && L->s->pos[d]) ? L->s->pos[d] : ""; p++; }
    else if (isalpha((unsigned char)*p) || *p == '_') {
        size_t n = 0; while ((isalnum((unsigned char)*p) || *p == '_') && n < sizeof name - 1) name[n++] = *p++;
        name[n] = 0; val = shell_get_var(L->s, name);
    } else { lex_putc(L, '$', quoted); *pp = p; return; }
    emit(L, val ? val : "", quoted);
    free(owned);
    *pp = p;
}

static char *unescape_backtick(const char *s, size_t n) {
    char *o = xmalloc(n + 1); size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\\' && i + 1 < n && strchr("`\\$", s[i + 1])) i++;
        o[k++] = s[i];
    }
    o[k] = 0; return o;
}

static int cmd_empty(const cmd_t *c) {
    return c->argv.n == 0 && !c->in_file && !c->out_file && !c->err_file && !c->err_to_out;
}
static void cmd_free(cmd_t *c) { sl_free(&c->argv); free(c->in_file); free(c->out_file); free(c->err_file); }
static void cmd_reset(cmd_t *c) { memset(c, 0, sizeof *c); sl_init(&c->argv); }

static void pl_push(pipeline_t *pl, cmd_t *c) {
    if (pl->n == pl->cap) { pl->cap = pl->cap ? pl->cap * 2 : 4; pl->cmds = xrealloc(pl->cmds, (size_t)pl->cap * sizeof(cmd_t)); }
    pl->cmds[pl->n++] = *c;
    cmd_reset(c);
}

void pipeline_free(pipeline_t *pl) {
    for (int i = 0; i < pl->n; i++) cmd_free(&pl->cmds[i]);
    free(pl->cmds); memset(pl, 0, sizeof *pl);
}

int parse_pipeline(shell_t *s, const char *stmt, pipeline_t *pl) {
    memset(pl, 0, sizeof *pl);
    lex_t L; memset(&L, 0, sizeof L);
    L.s = s; cmd_reset(&L.cur); buf_init(&L.val); buf_init(&L.pat);
    const char *p = stmt; int bad = 0;

    while (*p && !bad) {
        unsigned char c = (unsigned char)*p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { lex_flush(&L); p++; continue; }
        if (c == '#' && !L.active) break;                              /* comment */
        if (c == '|') {
            lex_flush(&L);
            if (L.pending || cmd_empty(&L.cur)) { syntax(s, "|"); bad = 1; break; }
            pl_push(pl, &L.cur); p++; continue;
        }
        if (!L.active && strncmp(p, "2>&1", 4) == 0 &&
            (p[4] == 0 || isspace((unsigned char)p[4]) || strchr(";&|<>", p[4]))) {
            L.cur.err_to_out = 1; p += 4; continue;
        }
        if (!L.active && c == '2' && p[1] == '>') {
            if (L.pending) { syntax(s, "2>"); bad = 1; break; }
            if (p[2] == '>') { L.pending = 5; p += 3; } else { L.pending = 4; p += 2; }
            continue;
        }
        if (c == '<' || c == '>') {
            lex_flush(&L);
            if (L.pending) { syntax(s, c == '<' ? "<" : ">"); bad = 1; break; }
            if (c == '<') { L.pending = 1; p++; }
            else if (p[1] == '>') { L.pending = 3; p += 2; }
            else { L.pending = 2; p++; }
            continue;
        }
        if (c == '\\') {
            if (p[1] == '\n') { p += 2; continue; }                    /* line continuation */
            if (p[1]) { lex_putc(&L, p[1], 1); p += 2; } else p++;
            continue;
        }
        if (c == '\'') {
            const char *e = skip_single(p + 1);
            if (!e) { syntax(s, "'"); bad = 1; break; }
            L.active = 1;
            for (const char *q = p + 1; q < e; q++) lex_putc(&L, *q, 1);
            p = e + 1; continue;
        }
        if (c == '"') {
            L.active = 1; p++;
            while (*p && *p != '"') {
                if (*p == '\\' && p[1]) {
                    char n = p[1];
                    if (n == '\n') {
#if DQ_BACKSLASH_NEWLINE_KEEPS_NEWLINE
                        lex_putc(&L, '\n', 1);
#endif
                        p += 2; continue;
                    }
                    if (n == '$' || n == '`' || n == '"' || n == '\\') { lex_putc(&L, n, 1); p += 2; continue; }
                    lex_putc(&L, '\\', 1); p++; continue;
                }
                if (*p == '$') { expand_dollar(&L, &p, 1); continue; }
                if (*p == '`') {
                    const char *e = skip_backtick(p + 1);
                    if (!e) { syntax(s, "`"); bad = 1; break; }
                    char *inner = unescape_backtick(p + 1, (size_t)(e - p - 1));
                    emit_capture(&L, inner, 1); free(inner); p = e + 1; continue;
                }
                lex_putc(&L, *p, 1); p++;
            }
            if (bad) break;
            if (*p != '"') { syntax(s, "\""); bad = 1; break; }
            p++; continue;
        }
        if (c == '$') { expand_dollar(&L, &p, 0); continue; }
        if (c == '`') {
            const char *e = skip_backtick(p + 1);
            if (!e) { syntax(s, "`"); bad = 1; break; }
            char *inner = unescape_backtick(p + 1, (size_t)(e - p - 1));
            emit_capture(&L, inner, 0); free(inner); p = e + 1; continue;
        }
        if (c == '~' && !L.active && (p[1] == 0 || p[1] == '/' || isspace((unsigned char)p[1]))) {
            const char *h = shell_get_var(s, "HOME");
            for (; h && *h; h++) lex_putc(&L, *h, 1);
            if (!h || L.val.len == 0) lex_putc(&L, '~', 1);
            p++; continue;
        }
        lex_putc(&L, (char)c, 0); p++;
    }

    if (!bad) lex_flush(&L);
    if (!bad && L.pending) { syntax(s, "newline"); bad = 1; }
    if (!bad) {
        if (cmd_empty(&L.cur)) { if (pl->n > 0) { syntax(s, "|"); bad = 1; } }
        else pl_push(pl, &L.cur);
    }
    if (!bad && pl->n > 1)
        for (int i = 0; i < pl->n; i++) if (pl->cmds[i].argv.n == 0) { syntax(s, "|"); bad = 1; break; }

    cmd_free(&L.cur); buf_free(&L.val); buf_free(&L.pat);
    if (bad) { pipeline_free(pl); return -1; }
    return 0;
}
