// The SNES as the game sees it: a model of the registers, VRAM, CGRAM and
// OAM the game writes, which the renderer (render.iwram.c) turns into the
// GBA's picture each frame.
#pragma once
#include <stdint.h>
#include <stdbool.h>

enum { kSnesLines = 224 };

// Registers that HDMA or the mid-frame IRQ can change from line to line.
typedef struct LineRegs {
  uint16_t hofs[4], vofs[4];
  uint8_t wh[4];          // window 1 left/right, window 2 left/right
  uint16_t fixed_color;   // COLDATA, as a BGR555 color
  uint8_t tm, ts;
} __attribute__((aligned(4))) LineRegs;

typedef struct SnesPpu {
  uint8_t inidisp, obsel, bgmode, mosaic;
  uint8_t bgsc[4], bgnba[2];
  uint8_t w12sel, w34sel, wobjsel, wbglog, wobjlog;
  uint8_t tmw, tsw, cgwsel, cgadsub, setini;
  uint8_t m7sel;
  uint8_t scroll_prev, scroll_prev2;
  uint8_t vmain;
  uint16_t vmadd;
  uint16_t vram_read_buf;
  uint8_t cgadd, cg_latch;
  bool cg_second;
  uint16_t oamadd, oam_ptr;
  uint8_t oam_latch;
  int16_t m7a;
  uint8_t m7b_latch;
  int16_t m7b;
  LineRegs cur;           // the registers as the game last wrote them
} SnesPpu;

typedef struct SnesDmaChannel {
  uint8_t dmap, bbad;
  uint16_t a1t;
  uint8_t a1b;
  uint16_t das;           // DMA size, or HDMA indirect address
  uint8_t dasb;           // HDMA indirect bank
  uint16_t a2a;           // HDMA table address
  uint8_t ntrl;           // HDMA line counter
  bool do_transfer, terminated;
} SnesDmaChannel;

typedef struct Snes {
  SnesPpu ppu;
  SnesDmaChannel dma[8];
  uint8_t nmitimen, hdmaen;
  uint16_t vtime, htime;
  uint8_t wrmpya;
  uint16_t rddiv, rdmpy;
  uint16_t wrdiv;
  bool timeup, rdnmi;
  uint16_t joy1, joy2;    // auto-read joypad words, SNES bit order
  int line;               // the line the game's code is running at
} Snes;

extern Snes g_snes_state;
#define g_snes (&g_snes_state)

// SNES memory, in EWRAM.
extern uint16_t g_snes_vram[0x8000];
extern uint16_t g_snes_cgram[0x100];
extern uint8_t g_snes_oam[0x220];

// Changes since the renderer last looked: a bit per 16 VRAM words, a bit per
// 16-color palette, and a flag for OAM.
extern uint32_t g_vram_dirty[0x8000 / 16 / 32];
extern uint16_t g_cgram_dirty;
extern bool g_oam_dirty;

// The registers of each line of the last frame, filled by SnesRunLines: the
// distinct states, in order, and each line's index into them.
enum { kMaxLineStates = kSnesLines + 1 };
extern LineRegs g_line_states[kMaxLineStates];
extern uint8_t g_line_state[kSnesLines];
extern bool g_line_regs_dirty;
// Each line's scroll registers, apart from the states (scroll changes on
// other lines than the rest): the distinct scroll records, in order (for
// each layer, hofs | vofs << 16), and each line's index into them. Layers
// whose scroll HDMA writes every line are baked: bit k of g_scroll_baked
// means layer k's hofs is g_scroll_bake[k][line], bit 4 + k its vofs is
// g_scroll_bake[4 + k][line].
extern uint32_t g_scroll_states[kSnesLines + 1][4];
extern uint8_t g_line_scroll_state[kSnesLines];
extern uint8_t g_scroll_baked;
extern uint16_t g_scroll_bake[8][kSnesLines];
extern bool g_scroll_dirty;

// A line's scroll for a layer: hofs | vofs << 16.
static inline uint32_t LineScroll(int line, int layer) {
  uint32_t v = g_scroll_states[g_line_scroll_state[line]][layer];
  if (g_scroll_baked & (1 << layer)) v = (v & 0xffff0000) | g_scroll_bake[layer][line];
  if (g_scroll_baked & (16 << layer)) v = (v & 0xffff) | (uint32_t)g_scroll_bake[4 + layer][line] << 16;
  return v;
}
// Which line registers changed anywhere in the frame: bits of kLineChanged.
extern uint32_t g_line_changed;
enum {
  kLineChanged_Hofs0 = 1 << 0,  // ... Hofs3 = 1 << 3
  kLineChanged_Vofs0 = 1 << 4,  // ... Vofs3 = 1 << 7
  kLineChanged_Window = 1 << 8,
  kLineChanged_Fixed = 1 << 9,
  kLineChanged_TmTs = 1 << 10,
};

void SnesReset(void);
// Runs the frame's lines: HDMA and the game's IRQ handler (irq_func), as a
// SNES would draw them, recording each line's registers.
void SnesRunLines(void (*irq_func)(void));

// The SNES address space, for DMA sources and the game's long pointers.
uint8_t *SnesPtr(uint32_t addr);
void SnesVramWrite(uint16_t addr, uint16_t value);
