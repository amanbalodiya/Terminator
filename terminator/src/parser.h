/* parser.h - PART 2: completeness check, statement splitting, tokenising + expansion */
#ifndef PARSER_H
#define PARSER_H
#include "common.h"

typedef struct {
    strlist_t argv;
    char *in_file, *out_file, *err_file;
    int append, err_append, err_to_out;      /* >> , 2>> , 2>&1 */
} cmd_t;

typedef struct { cmd_t *cmds; int n, cap; int background; } pipeline_t;

enum { T_NONE = 0, T_SEMI, T_BG, T_AND, T_OR };   /* statement terminators */

/* 1 if the text is not finished yet (open quote, trailing '\', open $( or `, trailing | && ||) */
int line_incomplete(const char *s);

/* find end of first statement in s. *term says which terminator stopped it */
const char *next_statement_end(const char *s, int *term);

/* quote/substitution skippers (p points just AFTER the opener; return pointer to the closer or NULL) */
const char *skip_single(const char *p);
const char *skip_double(const char *p);
const char *skip_backtick(const char *p);
const char *skip_dollar_paren(const char *p);

/* tokenise + expand ($VAR, $?, $(..), `..`, ~, globs) one statement. 0 ok, -1 syntax error (reported) */
int  parse_pipeline(shell_t *s, const char *stmt, pipeline_t *pl);
void pipeline_free(pipeline_t *pl);
#endif
