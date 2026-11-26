// GBA hardware registers and helpers.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define REG16(a) (*(volatile uint16_t *)(a))
#define REG32(a) (*(volatile uint32_t *)(a))

#define REG_DISPCNT   REG16(0x04000000)
#define REG_DISPSTAT  REG16(0x04000004)
#define REG_VCOUNT    REG16(0x04000006)
#define REG_BGCNT(n)  REG16(0x04000008 + 2 * (n))
#define REG_BGHOFS(n) REG16(0x04000010 + 4 * (n))
#define REG_BGVOFS(n) REG16(0x04000012 + 4 * (n))
#define REG_WIN0H     REG16(0x04000040)
#define REG_WIN1H     REG16(0x04000042)
#define REG_WIN0V     REG16(0x04000044)
#define REG_WIN1V     REG16(0x04000046)
#define REG_WININ     REG16(0x04000048)
#define REG_WINOUT    REG16(0x0400004A)
#define REG_MOSAIC    REG16(0x0400004C)
#define REG_BLDCNT    REG16(0x04000050)
#define REG_BLDALPHA  REG16(0x04000052)
#define REG_BLDY      REG16(0x04000054)

#define REG_SOUNDCNT_L REG16(0x04000080)
#define REG_SOUNDCNT_H REG16(0x04000082)
#define REG_SOUNDCNT_X REG16(0x04000084)
#define REG_SOUNDBIAS  REG16(0x04000088)
#define REG_FIFO_A     0x040000A0
#define REG_FIFO_B     0x040000A4

#define REG_DMA_SAD(n)  REG32(0x040000B0 + 12 * (n))
#define REG_DMA_DAD(n)  REG32(0x040000B4 + 12 * (n))
#define REG_DMA_CNT(n)  REG32(0x040000B8 + 12 * (n))
#define REG_DMA_CNT_H(n) REG16(0x040000BA + 12 * (n))

#define REG_TM_CNT_L(n) REG16(0x04000100 + 4 * (n))
#define REG_TM_CNT_H(n) REG16(0x04000102 + 4 * (n))

#define REG_KEYINPUT  REG16(0x04000130)
#define REG_IE        REG16(0x04000200)
#define REG_IF        REG16(0x04000202)
#define REG_WAITCNT   REG16(0x04000204)
#define REG_IME       REG16(0x04000208)
#define REG_BIOS_IF   REG16(0x03007FF8)

#define PAL_BG   ((volatile uint16_t *)0x05000000)
#define PAL_OBJ  ((volatile uint16_t *)0x05000200)
#define VRAM     ((volatile uint16_t *)0x06000000)
#define VRAM_OBJ ((volatile uint16_t *)0x06010000)
#define OAM      ((volatile uint16_t *)0x07000000)
#define SRAM     ((volatile uint8_t *)0x0E000000)

enum {
  DCNT_MODE0 = 0, DCNT_MODE1 = 1, DCNT_OBJ_2D = 0, DCNT_OBJ_1D = 0x40,
  DCNT_BLANK = 0x80, DCNT_BG0 = 0x100, DCNT_BG1 = 0x200, DCNT_BG2 = 0x400,
  DCNT_BG3 = 0x800, DCNT_OBJ = 0x1000, DCNT_WIN0 = 0x2000, DCNT_WIN1 = 0x4000,
  DCNT_WINOBJ = 0x8000,
};
enum {
  DSTAT_VBL_IRQ = 8, DSTAT_HBL_IRQ = 16, DSTAT_VCT_IRQ = 32,
};
enum {
  IRQ_VBLANK = 1, IRQ_HBLANK = 2, IRQ_VCOUNT = 4, IRQ_TIMER0 = 8, IRQ_TIMER1 = 16,
  IRQ_TIMER2 = 32, IRQ_TIMER3 = 64, IRQ_DMA0 = 0x100, IRQ_DMA1 = 0x200,
  IRQ_DMA2 = 0x400, IRQ_DMA3 = 0x800, IRQ_KEYPAD = 0x1000,
};
enum {
  DMA_DST_INC = 0, DMA_DST_DEC = 0x200000, DMA_DST_FIXED = 0x400000, DMA_DST_RELOAD = 0x600000,
  DMA_SRC_INC = 0, DMA_SRC_DEC = 0x800000, DMA_SRC_FIXED = 0x1000000,
  DMA_REPEAT = 0x2000000, DMA_16 = 0, DMA_32 = 0x4000000,
  DMA_NOW = 0, DMA_AT_VBLANK = 0x10000000, DMA_AT_HBLANK = 0x20000000, DMA_AT_FIFO = 0x30000000,
  DMA_IRQ = 0x40000000, DMA_ENABLE = 0x80000000u,
};
enum {
  KEY_A = 1, KEY_B = 2, KEY_SELECT = 4, KEY_START = 8, KEY_RIGHT = 16, KEY_LEFT = 32,
  KEY_UP = 64, KEY_DOWN = 128, KEY_R = 256, KEY_L = 512,
};

#define IWRAM_CODE __attribute__((section(".iwram"), target("arm"), noinline))
#define IWRAM_DATA __attribute__((section(".iwram")))
#define IWRAM_BSS __attribute__((section(".iwram_bss")))
#define EWRAM_BSS __attribute__((section(".ewram_bss")))
// Moves a cold function in a *.iwram.c file back to ROM, as Thumb.
#define ROM_CODE __attribute__((section(".text.rom"), target("thumb"), noinline))

static inline void DmaCopy32(int ch, void *dst, const void *src, uint32_t bytes) {
  REG_DMA_SAD(ch) = (uint32_t)src;
  REG_DMA_DAD(ch) = (uint32_t)dst;
  REG_DMA_CNT(ch) = (bytes >> 2) | DMA_32 | DMA_ENABLE;
}
static inline void DmaCopy16(int ch, void *dst, const void *src, uint32_t bytes) {
  REG_DMA_SAD(ch) = (uint32_t)src;
  REG_DMA_DAD(ch) = (uint32_t)dst;
  REG_DMA_CNT(ch) = (bytes >> 1) | DMA_16 | DMA_ENABLE;
}
static inline void DmaFill32(int ch, void *dst, const volatile uint32_t *val, uint32_t bytes) {
  REG_DMA_SAD(ch) = (uint32_t)val;
  REG_DMA_DAD(ch) = (uint32_t)dst;
  REG_DMA_CNT(ch) = (bytes >> 2) | DMA_32 | DMA_SRC_FIXED | DMA_ENABLE;
}

// Cycle counting with timers 2 and 3 cascaded (timer 0 and 1 are for sound).
static inline uint32_t CycleCount(void) {
  uint32_t hi = REG_TM_CNT_L(3), lo = REG_TM_CNT_L(2), hi2 = REG_TM_CNT_L(3);
  if (hi != hi2) lo = REG_TM_CNT_L(2);
  return (hi2 << 16) | lo;
}
