#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct __circle_file FILE;
extern FILE *stderr;
int snprintf(char *, size_t, const char *, ...);
int vsnprintf(char *, size_t, const char *, __builtin_va_list);
FILE *fopen(const char *, const char *);
char *fgets(char *, int, FILE *);
int fclose(FILE *);
#ifdef __cplusplus
}
#endif
#define printf(...) (0)
#define fprintf(...) (0)
