#pragma once
#include <stddef.h>
#include <stdarg.h>
typedef struct FILE FILE;
#define stderr ((FILE *)2)
#define stdout ((FILE *)1)
// Output goes to mGBA's debug log (see gba/libc/stdio.c).
int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int fprintf(FILE *f, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int snprintf(char *buf, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int sprintf(char *buf, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int puts(const char *s);
