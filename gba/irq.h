#pragma once
#include <stdint.h>
extern volatile uint32_t g_vblank_count;
void IrqInit(void);
static inline void VBlankIntrWait(void) {
  __asm__ volatile("swi 5" ::: "r0", "r1", "r2", "r3", "memory");
}
