// The cartridge's assets, appended to the engine by tools/make_cartridge.py:
// a directory ("SMASDATA", a count, then tag/offset/size per asset) at the
// first 256-byte boundary after the engine's own ROM.
#include <string.h>
#include "platform.h"

extern const char __rom_end[];

typedef struct AssetEntry { uint32_t tag, offset, size; } AssetEntry;

static const uint8_t *AssetDirectory(void) {
  uintptr_t p = ((uintptr_t)__rom_end + 255) & ~(uintptr_t)255;
  const uint8_t *d = (const uint8_t *)p;
  return memcmp(d, "SMASDATA", 8) == 0 ? d : NULL;
}

const void *FindAsset(uint32_t tag, uint32_t *size) {
  const uint8_t *d = AssetDirectory();
  if (!d) return NULL;
  uint32_t n = *(const uint32_t *)(d + 8);
  const AssetEntry *e = (const AssetEntry *)(d + 12);
  for (uint32_t i = 0; i < n; i++) {
    if (e[i].tag == tag) {
      if (size) *size = e[i].size;
      return d + e[i].offset;
    }
  }
  return NULL;
}
