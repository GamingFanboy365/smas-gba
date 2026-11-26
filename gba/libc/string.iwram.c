// memcpy, memset and friends, as ARM code in IWRAM. The game copies a lot of
// small buffers; word copies when both pointers are aligned the same way.
#include <string.h>
#include <stdint.h>

void *memcpy(void *dst, const void *src, size_t n) {
  uint8_t *d = dst;
  const uint8_t *s = src;
  if (n >= 8 && (((uintptr_t)d ^ (uintptr_t)s) & 3) == 0) {
    while ((uintptr_t)d & 3) *d++ = *s++, n--;
    uint32_t *dw = (uint32_t *)d;
    const uint32_t *sw = (const uint32_t *)s;
    for (; n >= 16; n -= 16) {
      uint32_t a = sw[0], b = sw[1], c = sw[2], e = sw[3];
      dw[0] = a, dw[1] = b, dw[2] = c, dw[3] = e;
      dw += 4, sw += 4;
    }
    for (; n >= 4; n -= 4) *dw++ = *sw++;
    d = (uint8_t *)dw, s = (const uint8_t *)sw;
  } else if (n >= 8 && (((uintptr_t)d ^ (uintptr_t)s) & 1) == 0) {
    if ((uintptr_t)d & 1) *d++ = *s++, n--;
    uint16_t *dh = (uint16_t *)d;
    const uint16_t *sh = (const uint16_t *)s;
    for (; n >= 2; n -= 2) *dh++ = *sh++;
    d = (uint8_t *)dh, s = (const uint8_t *)sh;
  }
  while (n--) *d++ = *s++;
  return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
  uint8_t *d = dst;
  const uint8_t *s = src;
  if (d <= s || d >= s + n)
    return memcpy(dst, src, n);
  d += n, s += n;
  while (n--) *--d = *--s;
  return dst;
}

void *memset(void *dst, int c, size_t n) {
  uint8_t *d = dst;
  uint8_t v = (uint8_t)c;
  if (n >= 8) {
    while ((uintptr_t)d & 3) *d++ = v, n--;
    uint32_t w = v * 0x01010101u;
    uint32_t *dw = (uint32_t *)d;
    for (; n >= 16; n -= 16) {
      dw[0] = w, dw[1] = w, dw[2] = w, dw[3] = w;
      dw += 4;
    }
    for (; n >= 4; n -= 4) *dw++ = w;
    d = (uint8_t *)dw;
  }
  while (n--) *d++ = v;
  return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
  const uint8_t *p = a, *q = b;
  for (; n; n--, p++, q++)
    if (*p != *q) return *p - *q;
  return 0;
}

// The EABI names clang may use for struct copies and clears.
void __aeabi_memcpy(void *d, const void *s, size_t n) { memcpy(d, s, n); }
void __aeabi_memcpy4(void *d, const void *s, size_t n) { memcpy(d, s, n); }
void __aeabi_memcpy8(void *d, const void *s, size_t n) { memcpy(d, s, n); }
void __aeabi_memmove(void *d, const void *s, size_t n) { memmove(d, s, n); }
void __aeabi_memmove4(void *d, const void *s, size_t n) { memmove(d, s, n); }
void __aeabi_memset(void *d, size_t n, int c) { memset(d, c, n); }
void __aeabi_memset4(void *d, size_t n, int c) { memset(d, c, n); }
void __aeabi_memclr(void *d, size_t n) { memset(d, 0, n); }
void __aeabi_memclr4(void *d, size_t n) { memset(d, 0, n); }
void __aeabi_memclr8(void *d, size_t n) { memset(d, 0, n); }
