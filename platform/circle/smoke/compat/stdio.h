/* Smoke-only diagnostics bridge; not a general libc implementation. */
#pragma once
#include <stdarg.h>
typedef struct tiny386_smoke_file FILE;
extern FILE *stderr;
int fprintf(FILE *, const char *, ...);
FILE *freopen(const char *, const char *, FILE *);
void setlinebuf(FILE *);
FILE *fopen(const char *, const char *);
char *fgets(char *, int, FILE *);
int fclose(FILE *);
