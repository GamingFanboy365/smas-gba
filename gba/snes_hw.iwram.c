// The SNES registers, DMA and HDMA, as the game's WriteReg/ReadReg calls see
// them. VRAM, CGRAM and OAM writes land in copies of the SNES's memories and
// mark what changed; the renderer converts those to the GBA's formats.
//
// A frame runs like this (see main.c): the game's main loop and its NMI
// handler run first, writing registers and DMAing graphics, all "before line
// 0". Then SnesRunLines walks the 224 visible lines as the SNES would draw
// them, running HDMA at the end of each line and the game's IRQ handler at
// its timer line, and records each line's scroll, window and fixed-color
// registers. The timing follows the PC reference's emulator (LakeSnes):
// HDMA starts at line 0, transfers at the end of every line from 0 to 224,
// and a V-timer IRQ fires at the end of line VTIME + 1; both affect the line
// after. Screen row r is line r + 1.
#include <string.h>
#include "snes.h"
#include "gba_hw.h"
#include "platform.h"
#include "../game/src/snes/snes_regs.h"

Snes g_snes_state;
EWRAM_BSS uint16_t g_snes_vram[0x8000];
EWRAM_BSS uint16_t g_snes_cgram[0x100];
EWRAM_BSS uint8_t g_snes_oam[0x220];
uint32_t g_vram_dirty[0x8000 / 16 / 32];
uint16_t g_cgram_dirty;
bool g_oam_dirty;
EWRAM_BSS LineRegs g_line_states[kMaxLineStates];
EWRAM_BSS uint8_t g_line_state[kSnesLines];
uint32_t g_line_changed;
bool g_line_regs_dirty;

static uint8_t g_open_bus_dummy[4];

ROM_CODE void SnesReset(void) {
  memset(g_snes, 0, sizeof(*g_snes));
  g_snes->ppu.inidisp = 0x80;
  g_snes->ppu.vmain = 0x80;
  memset(g_snes_vram, 0, sizeof(g_snes_vram));
  memset(g_snes_cgram, 0, sizeof(g_snes_cgram));
  memset(g_snes_oam, 0, sizeof(g_snes_oam));
  memset(g_vram_dirty, 0xff, sizeof(g_vram_dirty));
  g_cgram_dirty = 0xffff;
  g_oam_dirty = true;
}

uint8_t *SnesPtr(uint32_t addr) {
  uint32_t bank = (addr >> 16) & 0xff, off = addr & 0xffff;
  if (bank == 0x7e || bank == 0x7f)
    return &g_ram[addr & 0x1ffff];
  if (off >= 0x8000)
    return (uint8_t *)&g_rom[(((bank & 0x7f) << 15) | (off & 0x7fff)) & (g_rom_size - 1)];
  if ((bank & 0x7f) < 0x40 && off < 0x2000)
    return &g_ram[off];
  if ((bank & 0x7f) >= 0x70 && (bank & 0x7f) < 0x7e)
    return &g_sram[off & (kSramSize - 1)];
  return g_open_bus_dummy;
}

static inline void MarkVramDirty(uint32_t addr) {
  g_vram_dirty[addr >> 9] |= 1u << ((addr >> 4) & 31);
}

void SnesVramWrite(uint16_t addr, uint16_t value) {
  addr &= 0x7fff;
  if (g_snes_vram[addr] != value) {
    g_snes_vram[addr] = value;
    MarkVramDirty(addr);
  }
}

static const uint8_t kVramIncr[4] = { 1, 32, 128, 128 };

static void PpuWrite(uint8_t reg, uint8_t val) {
  SnesPpu *p = &g_snes->ppu;
  switch (reg) {
  case 0x00: p->inidisp = val; break;
  case 0x01: p->obsel = val; break;
  case 0x02: p->oamadd = (p->oamadd & 0x100) | val; p->oam_ptr = p->oamadd << 1; break;
  case 0x03: p->oamadd = (p->oamadd & 0xff) | (val & 1) << 8; p->oam_ptr = p->oamadd << 1; break;
  case 0x04: {
    uint16_t ptr = p->oam_ptr & 0x3ff;
    if (ptr < 0x200) {
      if (!(ptr & 1)) {
        p->oam_latch = val;
      } else if (g_snes_oam[ptr - 1] != p->oam_latch || g_snes_oam[ptr] != val) {
        g_snes_oam[ptr - 1] = p->oam_latch;
        g_snes_oam[ptr] = val;
        g_oam_dirty = true;
      }
    } else if (g_snes_oam[0x200 + (ptr & 0x1f)] != val) {
      g_snes_oam[0x200 + (ptr & 0x1f)] = val;
      g_oam_dirty = true;
    }
    p->oam_ptr = ptr + 1;
    break;
  }
  case 0x05: p->bgmode = val; break;
  case 0x06: p->mosaic = val; break;
  case 0x07: case 0x08: case 0x09: case 0x0a: p->bgsc[reg - 7] = val; break;
  case 0x0b: case 0x0c: p->bgnba[reg - 0xb] = val; break;
  case 0x0d: case 0x0f: case 0x11: case 0x13: {
    int i = (reg - 0x0d) >> 1;
    uint16_t v = ((val << 8) | (p->scroll_prev & 0xf8) | (p->scroll_prev2 & 7)) & 0x3ff;
    if (p->cur.hofs[i] != v) {
      p->cur.hofs[i] = v;
      g_line_changed |= kLineChanged_Hofs0 << i, g_line_regs_dirty = true;
    }
    p->scroll_prev = p->scroll_prev2 = val;
    break;
  }
  case 0x0e: case 0x10: case 0x12: case 0x14: {
    int i = (reg - 0x0e) >> 1;
    uint16_t v = ((val << 8) | p->scroll_prev) & 0x3ff;
    if (p->cur.vofs[i] != v) {
      p->cur.vofs[i] = v;
      g_line_changed |= kLineChanged_Vofs0 << i, g_line_regs_dirty = true;
    }
    p->scroll_prev = val;
    break;
  }
  case 0x15: p->vmain = val; break;
  case 0x16: p->vmadd = (p->vmadd & 0xff00) | val; p->vram_read_buf = g_snes_vram[p->vmadd & 0x7fff]; break;
  case 0x17: p->vmadd = (p->vmadd & 0xff) | val << 8; p->vram_read_buf = g_snes_vram[p->vmadd & 0x7fff]; break;
  case 0x18: {
    uint16_t a = p->vmadd & 0x7fff;
    SnesVramWrite(a, (g_snes_vram[a] & 0xff00) | val);
    if (!(p->vmain & 0x80)) p->vmadd += kVramIncr[p->vmain & 3];
    break;
  }
  case 0x19: {
    uint16_t a = p->vmadd & 0x7fff;
    SnesVramWrite(a, (g_snes_vram[a] & 0xff) | val << 8);
    if (p->vmain & 0x80) p->vmadd += kVramIncr[p->vmain & 3];
    break;
  }
  case 0x1a: p->m7sel = val; break;
  case 0x1b: p->m7a = val << 8 | p->m7b_latch; p->m7b_latch = val; break;
  case 0x1c: p->m7b = val << 8 | p->m7b_latch; p->m7b_latch = val; break;
  case 0x1d: case 0x1e: case 0x1f: case 0x20: p->m7b_latch = val; break;
  case 0x21: p->cgadd = val; p->cg_second = false; break;
  case 0x22:
    if (!p->cg_second) {
      p->cg_latch = val;
    } else {
      uint16_t c = (val << 8 | p->cg_latch) & 0x7fff;
      if (g_snes_cgram[p->cgadd] != c) {
        g_snes_cgram[p->cgadd] = c;
        g_cgram_dirty |= 1 << (p->cgadd >> 4);
      }
      p->cgadd++;
    }
    p->cg_second = !p->cg_second;
    break;
  case 0x23: p->w12sel = val; break;
  case 0x24: p->w34sel = val; break;
  case 0x25: p->wobjsel = val; break;
  case 0x26: case 0x27: case 0x28: case 0x29:
    if (p->cur.wh[reg - 0x26] != val) {
      p->cur.wh[reg - 0x26] = val;
      g_line_changed |= kLineChanged_Window, g_line_regs_dirty = true;
    }
    break;
  case 0x2a: p->wbglog = val; break;
  case 0x2b: p->wobjlog = val; break;
  case 0x2c: if (p->cur.tm != val) { p->cur.tm = val; g_line_changed |= kLineChanged_TmTs; g_line_regs_dirty = true; } break;
  case 0x2d: if (p->cur.ts != val) { p->cur.ts = val; g_line_changed |= kLineChanged_TmTs; g_line_regs_dirty = true; } break;
  case 0x2e: p->tmw = val; break;
  case 0x2f: p->tsw = val; break;
  case 0x30: p->cgwsel = val; break;
  case 0x31: p->cgadsub = val; break;
  case 0x32: {
    uint16_t c = p->cur.fixed_color, v = val & 0x1f;
    if (val & 0x20) c = (c & ~0x001f) | v;
    if (val & 0x40) c = (c & ~0x03e0) | v << 5;
    if (val & 0x80) c = (c & ~0x7c00) | v << 10;
    if (c != p->cur.fixed_color) {
      p->cur.fixed_color = c;
      g_line_changed |= kLineChanged_Fixed;
      g_line_regs_dirty = true;
    }
    break;
  }
  case 0x33: p->setini = val; break;
  case 0x40: case 0x41: case 0x42: case 0x43:
    ApuWritePort(reg & 3, val);
    break;
  default:
    break;
  }
}

static uint8_t PpuRead(uint8_t reg) {
  SnesPpu *p = &g_snes->ppu;
  switch (reg) {
  case 0x34: case 0x35: case 0x36: {
    int32_t r = p->m7a * (int8_t)(p->m7b >> 8);
    return r >> ((reg - 0x34) * 8);
  }
  case 0x38: {
    uint16_t ptr = p->oam_ptr & 0x3ff;
    p->oam_ptr = ptr + 1;
    return ptr < 0x200 ? g_snes_oam[ptr] : g_snes_oam[0x200 + (ptr & 0x1f)];
  }
  case 0x39: {
    uint8_t v = p->vram_read_buf;
    if (!(p->vmain & 0x80)) {
      p->vram_read_buf = g_snes_vram[p->vmadd & 0x7fff];
      p->vmadd += kVramIncr[p->vmain & 3];
    }
    return v;
  }
  case 0x3a: {
    uint8_t v = p->vram_read_buf >> 8;
    if (p->vmain & 0x80) {
      p->vram_read_buf = g_snes_vram[p->vmadd & 0x7fff];
      p->vmadd += kVramIncr[p->vmain & 3];
    }
    return v;
  }
  case 0x3b: {
    uint16_t c = g_snes_cgram[p->cgadd];
    uint8_t v = p->cg_second ? c >> 8 : c;
    if (p->cg_second) p->cgadd++;
    p->cg_second = !p->cg_second;
    return v;
  }
  case 0x40: case 0x41: case 0x42: case 0x43:
    return ApuReadPort(reg & 3);
  default:
    return 0;
  }
}

// ---- DMA ----

static const uint8_t kBAdrOffsets[8][4] = {
  {0, 0, 0, 0}, {0, 1, 0, 1}, {0, 0, 0, 0}, {0, 0, 1, 1},
  {0, 1, 2, 3}, {0, 1, 0, 1}, {0, 0, 0, 0}, {0, 0, 1, 1},
};
static const uint8_t kTransferLength[8] = { 1, 2, 2, 4, 4, 4, 2, 4 };

static void DoDma(SnesDmaChannel *c) {
  uint32_t count = c->das ? c->das : 0x10000;
  uint8_t mode = c->dmap & 7;
  int step = (c->dmap & 8) ? 0 : (c->dmap & 16) ? -1 : 1;
  SnesPpu *p = &g_snes->ppu;

  // Fast paths for what the games do every frame, when the source doesn't
  // run off the end of its bank (the slow path below handles that).
  if (!(c->dmap & 0x80) && step == 1 && (uint32_t)c->a1t + count <= 0x10000) {
    const uint8_t *src = SnesPtr(c->a1b << 16 | c->a1t);
    // Words to VRAM through $2118/$2119.
    if (mode == 1 && c->bbad == 0x18 && !(count & 1) && (p->vmain & 0x8c) == 0x80) {
      uint32_t inc = kVramIncr[p->vmain & 3];
      uint16_t a = p->vmadd;
      if (!((uintptr_t)src & 1)) {
        const uint16_t *s16 = (const uint16_t *)src;
        for (uint32_t i = 0; i < count / 2; i++, a += inc) {
          uint16_t v = s16[i], va = a & 0x7fff;
          if (g_snes_vram[va] != v) {
            g_snes_vram[va] = v;
            MarkVramDirty(va);
          }
        }
      } else {
        for (uint32_t i = 0; i < count; i += 2, a += inc)
          SnesVramWrite(a, src[i] | src[i + 1] << 8);
      }
      p->vmadd = a;
      c->a1t += count;
      c->das = 0;
      return;
    }
    // OAM from the start, all of it.
    if (mode == 0 && c->bbad == 0x04 && p->oam_ptr == 0 && count == 0x220) {
      memcpy(g_snes_oam, src, 0x220);
      g_oam_dirty = true;
      p->oam_ptr = 0x220;
      c->a1t += count;
      c->das = 0;
      return;
    }
  }

  // A fill from the PPU's multiplier (the games clear RAM this way): the
  // value is the same for every byte.
  if ((c->dmap & 0x80) && c->bbad == 0x34 && mode == 0 && step == 1 &&
      (uint32_t)c->a1t + count <= 0x10000) {
    memset(SnesPtr(c->a1b << 16 | c->a1t), PpuRead(0x34), count);
    c->a1t += count;
    c->das = 0;
    return;
  }

  uint8_t i = 0;
  do {
    uint8_t breg = c->bbad + kBAdrOffsets[mode][i];
    i = (i + 1) & 3;
    if (i == kTransferLength[mode]) i = 0;
    uint8_t *ap = SnesPtr(c->a1b << 16 | c->a1t);
    if (c->dmap & 0x80)
      *ap = PpuRead(breg);
    else
      PpuWrite(breg, *ap);
    c->a1t += step;
  } while (--count);
  c->das = 0;
}

static void DoMdmaen(uint8_t val) {
  for (int i = 0; i < 8; i++)
    if (val & (1 << i))
      DoDma(&g_snes->dma[i]);
}

static void DmaRegWrite(uint16_t reg, uint8_t val) {
  SnesDmaChannel *c = &g_snes->dma[(reg >> 4) & 7];
  switch (reg & 0xf) {
  case 0: c->dmap = val; break;
  case 1: c->bbad = val; break;
  case 2: c->a1t = (c->a1t & 0xff00) | val; break;
  case 3: c->a1t = (c->a1t & 0xff) | val << 8; break;
  case 4: c->a1b = val; break;
  case 5: c->das = (c->das & 0xff00) | val; break;
  case 6: c->das = (c->das & 0xff) | val << 8; break;
  case 7: c->dasb = val; break;
  case 8: c->a2a = (c->a2a & 0xff00) | val; break;
  case 9: c->a2a = (c->a2a & 0xff) | val << 8; break;
  case 0xa: c->ntrl = val; break;
  }
}

ROM_CODE static uint8_t DmaRegRead(uint16_t reg) {
  SnesDmaChannel *c = &g_snes->dma[(reg >> 4) & 7];
  switch (reg & 0xf) {
  case 0: return c->dmap;
  case 1: return c->bbad;
  case 2: return c->a1t;
  case 3: return c->a1t >> 8;
  case 4: return c->a1b;
  case 5: return c->das;
  case 6: return c->das >> 8;
  case 7: return c->dasb;
  case 8: return c->a2a;
  case 9: return c->a2a >> 8;
  case 0xa: return c->ntrl;
  }
  return 0;
}

void WriteReg(uint16_t reg, uint8_t value) {
  if ((reg & 0xff00) == 0x2100) {
    PpuWrite(reg & 0xff, value);
    return;
  }
  Snes *s = g_snes;
  switch (reg) {
  case 0x4200: s->nmitimen = value; break;
  case 0x4202: s->wrmpya = value; break;
  case 0x4203: s->rdmpy = s->wrmpya * value; break;
  case 0x4204: s->wrdiv = (s->wrdiv & 0xff00) | value; break;
  case 0x4205: s->wrdiv = (s->wrdiv & 0xff) | value << 8; break;
  case 0x4206:
    if (value) {
      s->rddiv = s->wrdiv / value;
      s->rdmpy = s->wrdiv % value;
    } else {
      s->rddiv = 0xffff;
      s->rdmpy = s->wrdiv;
    }
    break;
  case 0x4207: s->htime = (s->htime & 0x100) | value; break;
  case 0x4208: s->htime = (s->htime & 0xff) | (value & 1) << 8; break;
  case 0x4209: s->vtime = (s->vtime & 0x100) | value; break;
  case 0x420a: s->vtime = (s->vtime & 0xff) | (value & 1) << 8; break;
  case 0x420b: DoMdmaen(value); break;
  case 0x420c: s->hdmaen = value; break;
  default:
    if (reg >= 0x4300 && reg < 0x4380)
      DmaRegWrite(reg, value);
    break;
  }
}

ROM_CODE void WriteRegWord(uint16_t reg, uint16_t value) {
  WriteReg(reg, (uint8_t)value);
  WriteReg(reg + 1, value >> 8);
}

uint8_t ReadReg(uint16_t reg) {
  if ((reg & 0xff00) == 0x2100)
    return PpuRead(reg & 0xff);
  Snes *s = g_snes;
  switch (reg) {
  case 0x4016: case 0x4017:
    return 0;
  case 0x4210: {
    uint8_t v = 0x02 | (s->rdnmi << 7);
    s->rdnmi = false;
    return v;
  }
  case 0x4211: {
    uint8_t v = s->timeup << 7;
    s->timeup = false;
    return v;
  }
  case 0x4212:
    // Always "in hblank, joypad read done": the game only waits for those.
    return 0x40 | (s->line == 0 ? 0x80 : 0);
  case 0x4214: return s->rddiv;
  case 0x4215: return s->rddiv >> 8;
  case 0x4216: return s->rdmpy;
  case 0x4217: return s->rdmpy >> 8;
  case 0x4218: return s->joy1;
  case 0x4219: return s->joy1 >> 8;
  case 0x421a: return s->joy2;
  case 0x421b: return s->joy2 >> 8;
  default:
    if (reg >= 0x4300 && reg < 0x4380)
      return DmaRegRead(reg);
    return 0;
  }
}

ROM_CODE uint16_t ReadRegWord(uint16_t reg) {
  return ReadReg(reg) | ReadReg(reg + 1) << 8;
}

// ---- HDMA and the line walk ----
// The channels' table and indirect pointers are resolved once per table
// entry, not per byte: tables don't cross a bank in these games.

static const uint8_t *s_hdma_table[8], *s_hdma_ind[8];

static uint8_t HdmaReadTable(int i) {
  SnesDmaChannel *c = &g_snes->dma[i];
  c->a2a++;
  return *s_hdma_table[i]++;
}

static void HdmaLoadIndirect(int i) {
  SnesDmaChannel *c = &g_snes->dma[i];
  c->das = HdmaReadTable(i);
  c->das |= HdmaReadTable(i) << 8;
  s_hdma_ind[i] = SnesPtr(c->dasb << 16 | c->das);
}

// Lines are processed per channel only where the channel does something:
// a transfer (a repeating entry's every line, another entry's first) or
// the step to its next table entry. In between, its line counter is caught
// up at once. s_next[i] is that line; s_last[i] the last one handled.
static int16_t s_next[8], s_last[8];
static int16_t s_next_any;

static void HdmaInit(void) {
  s_next_any = 0x7fff;
  for (int i = 0; i < 8; i++) {
    SnesDmaChannel *c = &g_snes->dma[i];
    c->terminated = false;
    s_next[i] = 0x7fff;
    if (!(g_snes->hdmaen & (1 << i))) {
      c->do_transfer = false;
      continue;
    }
    c->a2a = c->a1t;
    s_hdma_table[i] = SnesPtr(c->a1b << 16 | c->a2a);
    c->ntrl = HdmaReadTable(i);
    if (c->ntrl == 0) {
      c->terminated = true;
      continue;
    }
    if (c->dmap & 0x40)
      HdmaLoadIndirect(i);
    c->do_transfer = true;
    s_next[i] = 0;
    s_last[i] = -1;
    s_next_any = 0;
  }
}

static void HdmaStep(int i, int line) {
  SnesDmaChannel *c = &g_snes->dma[i];
  c->ntrl -= line - s_last[i] - 1;  // the idle lines since the last one
  s_last[i] = line;
  if (c->do_transfer) {
    uint8_t mode = c->dmap & 7;
    const uint8_t **src = (c->dmap & 0x40) ? &s_hdma_ind[i] : &s_hdma_table[i];
    int n = kTransferLength[mode];
    for (int j = 0; j < n; j++)
      PpuWrite(c->bbad + kBAdrOffsets[mode][j], *(*src)++);
    if (c->dmap & 0x40) c->das += n; else c->a2a += n;
  }
  c->ntrl--;
  c->do_transfer = c->ntrl & 0x80;
  if ((c->ntrl & 0x7f) == 0) {
    c->ntrl = HdmaReadTable(i);
    if (c->dmap & 0x40)
      HdmaLoadIndirect(i);
    if (c->ntrl == 0) c->terminated = true;
    c->do_transfer = true;
  }
  if (c->terminated)
    s_next[i] = 0x7fff;
  else
    s_next[i] = c->do_transfer ? line + 1 : line + (c->ntrl & 0x7f);
}

static void HdmaDoLine(int line) {
  int next = 0x7fff;
  for (int i = 0; i < 8; i++) {
    if (s_next[i] == line) HdmaStep(i, line);
    if (s_next[i] < next) next = s_next[i];
  }
  s_next_any = next;
}

void SnesRunLines(void (*irq_func)(void)) {
  Snes *s = g_snes;
  // Line 0: HDMA starts, and transfers for line 1 (screen row 0).
  s->line = 0;
  g_line_changed = 0;
  bool v_irq = (s->nmitimen & 0x30) == 0x20 && irq_func;
  s_next_any = 0x7fff;
  if (s->hdmaen) {
    HdmaInit();
    if (s_next_any == 0) HdmaDoLine(0);
  }
  int nstates = 0;
  g_line_regs_dirty = true;
  for (int line = 1; line <= kSnesLines; line++) {
    s->line = line;
    if (g_line_regs_dirty) {
      g_line_regs_dirty = false;
      uint32_t *d = (uint32_t *)&g_line_states[nstates++];
      const uint32_t *c = (const uint32_t *)&s->ppu.cur;
      for (unsigned k = 0; k < sizeof(LineRegs) / 4; k++) d[k] = c[k];
    }
    g_line_state[line - 1] = nstates - 1;
    if (line == s_next_any)
      HdmaDoLine(line);
    if (v_irq && line == s->vtime + 1) {
      s->timeup = true;
      irq_func();
    }
  }
  s->line = 0;
  s->rdnmi = true;
}
