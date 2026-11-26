// Interrupts: IrqDispatch is called by irq_entry (crt0.S) in system mode
// with interrupts off.
#include "gba_hw.h"
#include "audio.h"
#include "render.h"
#include "irq.h"

volatile uint32_t g_vblank_count;

void IrqDispatch(uint32_t flags) {
  if (flags & IRQ_VBLANK) {
    g_vblank_count++;
    RenderVBlank();
    AudioVBlank();
  }
  if (flags & IRQ_VCOUNT) {
    RenderMidFrame();
    AudioMix();
  }
}

void IrqInit(void) {
  REG_IME = 0;
  // VCount, near the top of the frame (RenderVBlank sets the line): the
  // HUD layer's change of priority, then the mixer.
  REG_DISPSTAT = DSTAT_VBL_IRQ | DSTAT_VCT_IRQ | (0 << 8);
  REG_IE = IRQ_VBLANK | IRQ_VCOUNT;
  REG_IF = 0xffff;
  REG_IME = 1;
}
