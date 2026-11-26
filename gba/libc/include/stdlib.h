#pragma once
#include <stddef.h>
void *malloc(size_t n);
void *calloc(size_t n, size_t m);
void free(void *p);
void *realloc(void *p, size_t n);
int abs(int x);
void abort(void) __attribute__((noreturn));
void exit(int code) __attribute__((noreturn));
int atoi(const char *s);
