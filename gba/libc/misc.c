// The rest of the small C library: strings, a bump allocator, printf to
// mGBA's debug log.
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

size_t strlen(const char *s) { const char *p = s; while (*p) p++; return p - s; }
int strcmp(const char *a, const char *b) {
  while (*a && *a == *b) a++, b++;
  return (uint8_t)*a - (uint8_t)*b;
}
int strncmp(const char *a, const char *b, size_t n) {
  for (; n; n--, a++, b++) {
    if (*a != *b || !*a) return (uint8_t)*a - (uint8_t)*b;
  }
  return 0;
}
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) {} return r; }
char *strchr(const char *s, int c) {
  for (;; s++) {
    if (*s == (char)c) return (char *)s;
    if (!*s) return NULL;
  }
}
int abs(int x) { return x < 0 ? -x : x; }
int atoi(const char *s) {
  int v = 0, neg = (*s == '-');
  if (neg || *s == '+') s++;
  while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
  return neg ? -v : v;
}

// Allocations are made once at startup and never freed.
extern char __heap_start[], __heap_end[];
static char *heap_cur;
void *malloc(size_t n) {
  if (!heap_cur) heap_cur = __heap_start;
  n = (n + 7) & ~7u;
  if (heap_cur + n > __heap_end) {
    printf("malloc(%u) failed", (unsigned)n);
    return NULL;
  }
  void *p = heap_cur;
  heap_cur += n;
  return p;
}
void *calloc(size_t n, size_t m) {
  void *p = malloc(n * m);
  if (p) memset(p, 0, n * m);
  return p;
}
void free(void *p) {}
void *realloc(void *p, size_t n) { return NULL; }
void abort(void) { printf("abort"); for (;;) {} }
void exit(int code) { printf("exit(%d)", code); for (;;) {} }

// mGBA's debug output: enable with 0xC0DE, write the text, then the level.
#define REG_DEBUG_ENABLE (*(volatile uint16_t *)0x04FFF780)
#define REG_DEBUG_FLAGS (*(volatile uint16_t *)0x04FFF700)
#define REG_DEBUG_STRING ((volatile char *)0x04FFF600)

// Whether there is anyone to print to: mGBA answers 0x1DEA. On hardware
// and other emulators printf does nothing, and costs nothing.
static bool DebugAvailable(void) {
  static uint8_t state;  // 0 unknown, 1 yes, 2 no
  if (!state) {
    REG_DEBUG_ENABLE = 0xC0DE;
    state = REG_DEBUG_ENABLE == 0x1DEA ? 1 : 2;
  }
  return state == 1;
}

static void DebugWrite(const char *s, size_t n) {
  if (!DebugAvailable()) return;
  if (n > 255) n = 255;
  for (size_t i = 0; i < n; i++) REG_DEBUG_STRING[i] = s[i];
  REG_DEBUG_STRING[n] = 0;
  REG_DEBUG_FLAGS = 3 | 0x100;  // level: info
}

static char *PutNum(char *p, char *end, uint32_t v, int base, int width, char pad, bool neg, bool upper) {
  char tmp[12];
  int n = 0;
  const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  do {
    // Division by 10 by reciprocal multiplication; bases 8 and 16 shift.
    uint32_t q = base == 16 ? v >> 4 : base == 8 ? v >> 3 : (uint32_t)(((uint64_t)v * 0xCCCCCCCDu) >> 35);
    tmp[n++] = digits[v - q * base];
    v = q;
  } while (v);
  if (neg) tmp[n++] = '-';
  for (int i = n; i < width; i++) if (p < end) *p++ = pad;
  while (n) if (p < end) *p++ = tmp[--n]; else n--;
  return p;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
  char *p = buf, *end = buf + (size ? size - 1 : 0);
  for (; *fmt; fmt++) {
    if (*fmt != '%') { if (p < end) *p++ = *fmt; continue; }
    fmt++;
    char pad = ' ';
    int width = 0;
    if (*fmt == '-') fmt++;
    if (*fmt == '0') pad = '0', fmt++;
    while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
    if (*fmt == '.') { fmt++; while (*fmt >= '0' && *fmt <= '9') fmt++; }
    while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z') fmt++;
    switch (*fmt) {
    case 'd': case 'i': {
      int v = va_arg(ap, int);
      p = PutNum(p, end, v < 0 ? -(uint32_t)v : (uint32_t)v, 10, width, pad, v < 0, false);
      break;
    }
    case 'u': p = PutNum(p, end, va_arg(ap, unsigned), 10, width, pad, false, false); break;
    case 'x': p = PutNum(p, end, va_arg(ap, unsigned), 16, width, pad, false, false); break;
    case 'X': p = PutNum(p, end, va_arg(ap, unsigned), 16, width, pad, false, true); break;
    case 'p': p = PutNum(p, end, (uintptr_t)va_arg(ap, void *), 16, 8, '0', false, false); break;
    case 'c': if (p < end) *p++ = (char)va_arg(ap, int); break;
    case 's': {
      const char *s = va_arg(ap, const char *);
      if (!s) s = "(null)";
      int n = (int)strlen(s);
      for (int i = n; i < width; i++) if (p < end) *p++ = ' ';
      while (*s) { if (p < end) *p++ = *s; s++; }
      break;
    }
    case '%': if (p < end) *p++ = '%'; break;
    default: if (p < end) *p++ = '?'; break;
    }
  }
  if (size) *p = 0;
  return (int)(p - buf);
}

int snprintf(char *buf, size_t n, const char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  int r = vsnprintf(buf, n, fmt, ap);
  va_end(ap);
  return r;
}
int sprintf(char *buf, const char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  int r = vsnprintf(buf, 256, fmt, ap);
  va_end(ap);
  return r;
}

static int VPrint(const char *fmt, va_list ap) {
  if (!DebugAvailable()) return 0;
  char buf[256];
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  // One log line per call; strip a trailing newline.
  int len = n < 255 ? n : 255;
  if (len && buf[len - 1] == '\n') len--;
  DebugWrite(buf, len);
  return n;
}
int printf(const char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  int r = VPrint(fmt, ap);
  va_end(ap);
  return r;
}
int fprintf(FILE *f, const char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  int r = VPrint(fmt, ap);
  va_end(ap);
  return r;
}
int puts(const char *s) { DebugWrite(s, strlen(s)); return 0; }
void GbaAssertFailed(const char *expr, const char *file, int line) {
  printf("assert failed: %s at %s:%d", expr, file, line);
}
