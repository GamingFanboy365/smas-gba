// The runtime functions snesrev's game code calls (src/common_rtl.h in the
// PC version), for the GBA.
#include <string.h>
#include <stdio.h>
#include "gba_hw.h"
#include "platform.h"
#include "snes.h"
#include "../game/src/types.h"

EWRAM_BSS uint8_t g_ram[0x20000];
const uint8_t *g_rom;
uint32_t g_rom_size;
uint8_t *g_sram;
bool g_use_my_apu_code = true;

uint8 *RomPtr(uint32 addr) {
  return (uint8 *)&g_rom[(((addr >> 16) << 15) | (addr & 0x7fff)) & (g_rom_size - 1)];
}

uint8 *IndirPtr(LongPtr ptr, uint16 offs) {
  uint32 a = ((ptr.bank << 16) | ptr.addr) + offs;
  if (((a >> 16) >= 0x7e && (a >> 16) <= 0x7f) || (a & 0xffff) < 0x2000)
    return &g_ram[a & 0x1ffff];
  return RomPtr(a);
}

void IndirWriteByte(LongPtr ptr, uint16 offs, uint8 value) {
  *IndirPtr(ptr, offs) = value;
}

void SetHiLo(uint8 *hi, uint8 *lo, uint16 v) {
  *hi = v >> 8;
  *lo = v;
}

void AddHiLo(uint8 *hi, uint8 *lo, uint16 v) {
  SetHiLo(hi, lo, ((*hi << 8) | *lo) + v);
}

bool Unreachable(void) {
  printf("Unreachable!");
  return false;
}

void MemCpy(void *dst, const void *src, int size) { memcpy(dst, src, size); }
