#ifndef CLI_H
#define CLI_H

#include <stddef.h>

#include "library.h"

/* Reads one line without its newline; returns 0 when input is closed. */
int cli_prompt_line(const char *prompt, char *buf, size_t size);
/* Like cli_prompt_line, but hides the typing. */
int cli_prompt_secret(const char *prompt, char *buf, size_t size);

/* Whole number in [min, max]; returns 1 on success. */
int cli_parse_int(const char *text, int min, int max, int *out);

/* The command shell: runs until "exit" or the end of input, then saves. */
void cli_run(Library *lib);

#endif
