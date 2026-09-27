#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void *malloc(size_t);
void *calloc(size_t, size_t);
void *realloc(void *, size_t);
void free(void *);
void abort(void);
long strtol(const char *, char **, int);
int atoi(const char *);
long atol(const char *);
char *strdup(const char *);
#ifdef __cplusplus
}
#endif
