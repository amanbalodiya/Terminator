/* executor.h - PART 3 */
#ifndef EXECUTOR_H
#define EXECUTOR_H
#include "parser.h"
#include "shell.h"
/* Run one pipeline (fork/exec/pipes/redirection/background). `text` is the raw statement,
 * used for job names. Returns the exit status. */
int exec_pipeline(shell_t *s, pipeline_t *pl, const char *text);
#endif
