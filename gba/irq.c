// Interrupts: IrqDispatch is called by irq_entry (crt0.S) in system mode
// with interrupts off.
#include "gba_hw.h"
#include "audio.h"
#include "irq.h"

volatile uint32_t g_vblank_count;

void IrqDispatch(uint32_t flags) {
  if (flags & IRQ_VBLANK) {
    g_vblank_count++;
    AudioVBlank();
  }
  if (flags & IRQ_VCOUNT)
    AudioMix();
}

void IrqInit(void) {
  REG_IME = 0;
  // VCount 0, the top of the frame: the mixer (after VBlank, when the frame
  // has been presented).
  REG_DISPSTAT = DSTAT_VBL_IRQ | DSTAT_VCT_IRQ | (0 << 8);
  REG_IE = IRQ_VBLANK | IRQ_VCOUNT;
  REG_IF = 0xffff;
  REG_IME = 1;
}
