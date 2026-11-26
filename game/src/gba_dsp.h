// The DSP interface snesrev's sound driver calls, on the GBA: the registers
// go to the GBA's mixer (gba/mixer.iwram.c) instead of an emulated S-DSP.
#pragma once
#include <stdint.h>
#include "snes/dsp_regs.h"

typedef struct Dsp Dsp;
typedef struct DspRegWriteHistory {
  uint32_t count;
  uint8_t addr[256];
  uint8_t val[256];
} DspRegWriteHistory;

Dsp *dsp_init(uint8_t *ram);
void dsp_reset(Dsp *dsp);
void dsp_write(Dsp *dsp, uint8_t adr, uint8_t val);
