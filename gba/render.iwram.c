// The renderer: turns the SNES's VRAM, CGRAM, OAM and registers (snes.h)
// into the GBA's backgrounds, sprites and per-line effects.
//
// GBA BG VRAM layout (mode 0):
//   0x0000-0x7fff  the SNES's BG1/BG2 4bpp tiles, tile n at GBA tile n.
//                  GBA tile 1023 is kept transparent (see kBlankTile12).
//   0x8000-0x97ff  the SNES's BG3 2bpp tiles 0-189, widened to 4bpp, using
//                  GBA palettes 8-15 (colors 0-3). Tile 190 is solid color
//                  15, tile 191 transparent.
//   0x9800-0x9fff  the tint layer's map: tile 190, palette 15, everywhere.
//   0xa000-0xffff  12 screenblocks: for each SNES layer, its whole map
//                  ("full") and a copy with only its priority tiles ("hi").
// A SNES layer whose tiles of both priorities matter is shown as two GBA
// layers, the full map below and the hi map above: the priority tiles are
// drawn twice, the top copy covering the bottom one exactly.
//
// OBJ VRAM: 2D mapping, 32 tiles a row. SNES sprite tile t (16 a row) is at
// row t / 16, column t % 16, and again at column t % 16 + 16, so 16x16
// sprites that wrap around the SNES's row still find their right half.
//
// Layer order: the SNES's front-to-back order of (layer, priority) planes
// for Mode 1 is listed, filtered to the planes the frame shows, and mapped
// onto the GBA's 4 priority levels (PlanLayers). In SMB1's levels BG2 and
// BG3 are on the sub screen, added only where the main screen (BG1, sprites)
// shows the backdrop; with a black backdrop that is the same as drawing them
// behind the main screen, which is what the plan does.
#include <string.h>
#include <stdio.h>
#include "gba_hw.h"
#include "snes.h"
#include "render.h"

enum {
  kBlankTile12 = 1023,
  kSolidTile3 = 190,     // all color 15: the tint layer's tile
  kBlankTile3 = 191,
  kTintSb = 19,          // the tint layer's map, after BG3's 192 tiles
  kTitleCursorTile = 0x22e, kTitleBlankTile = 0x024,
  kSbBase = 20,          // first screenblock of the maps
  kGbaLines = 160,
};

// Screenblock of each SNES layer's full and hi maps.
static inline int SbFull(int layer) { return kSbBase + layer * 4; }
static inline int SbHi(int layer) { return kSbBase + layer * 4 + 2; }

static uint32_t s_spread[256];  // bit 7-x of a byte to bit 4x of a word

// The layout the converted VRAM follows.
static uint16_t s_bg12_base, s_bg3_base, s_obj_base[2];
static uint16_t s_map_base[3];
static uint8_t s_map_size[3];
static bool s_layout_valid;

// Priority-tile counts per SNES layer and map row (rows 0-63).
EWRAM_BSS static uint16_t s_prio_rows[3][64];
static uint16_t s_prio_total[3];

// Per-line tables for HBlank DMA, double buffered: scroll registers for
// BG0-3 (8 halfwords) and the backdrop color. Entry kGbaLines repeats the
// last line, for the HBlank DMA after the last line.
typedef struct LineTable {
  uint16_t scroll[kGbaLines + 1][8];
  uint16_t backdrop[kGbaLines + 1];
} __attribute__((aligned(4))) LineTable;
IWRAM_BSS static LineTable s_tables[2];
static int s_back;

// What RenderPresent puts in the registers.
typedef struct FrameRegs {
  uint16_t dispcnt, bgcnt[4], bldcnt, bldalpha, bldy, mosaic;
  bool backdrop_per_line;
  bool blank;
  bool tint;              // the per-line color is the tint layer's, not the backdrop's
  uint8_t squash;
} FrameRegs;
static FrameRegs s_regs[2];
// The frame on screen, for RenderVBlank: its line table (NULL: none), where
// its per-line color goes, and whether that changes down the screen.
static const LineTable *volatile s_front;
static volatile uint16_t *s_front_color;
static bool s_front_per_line;
EWRAM_BSS static uint16_t s_oam[2][512] __attribute__((aligned(4)));
static bool s_oam_ready[2];

// ---- tiles and maps ----

ROM_CODE static void InitSpread(void) {
  for (int b = 0; b < 256; b++) {
    uint32_t r = 0;
    for (int x = 0; x < 8; x++)
      if (b & (0x80 >> x)) r |= 1u << (4 * x);
    s_spread[b] = r;
  }
}

// On the screens between levels (the full squash) the text layer's tiles
// lose rows 0 and 7. BG1/BG2's tiles are converted with them merged into
// rows 1 and 6: the inner row's pixel where it has one, the dropped row's
// where it's empty. BG3's font (the HUD, white over the sky) reads better
// left alone. The title keeps its text rows whole and needs none of it.
static uint8_t s_tiles_squashed;  // the kSquash mode the BG tiles are in

// a's pixels where a has one, b's elsewhere.
static inline uint32_t MergeRows(uint32_t a, uint32_t b) {
  uint32_t m = (a | a >> 1 | a >> 2 | a >> 3) & 0x11111111;
  m *= 0xf;
  return (a & m) | (b & ~m);
}

static void Convert4bpp(volatile uint32_t *dst, const uint16_t *src, bool merge) {
  const uint32_t *sp = s_spread;
  uint32_t rows[8];
  for (int y = 0; y < 8; y++) {
    uint32_t lo = src[y], hi = src[y + 8];
    rows[y] = sp[lo & 0xff] | sp[lo >> 8] << 1 | sp[hi & 0xff] << 2 | sp[hi >> 8] << 3;
  }
  if (merge) {
    rows[1] = MergeRows(rows[1], rows[0]);
    rows[6] = MergeRows(rows[6], rows[7]);
  }
  for (int y = 0; y < 8; y++) dst[y] = rows[y];
}

static void Convert2bpp(volatile uint32_t *dst, const uint16_t *src, bool merge) {
  const uint32_t *sp = s_spread;
  uint32_t rows[8];
  for (int y = 0; y < 8; y++) {
    uint32_t lo = src[y];
    rows[y] = sp[lo & 0xff] | sp[lo >> 8] << 1;
  }
  if (merge) {
    rows[1] = MergeRows(rows[0], rows[1]);
    rows[6] = MergeRows(rows[7], rows[6]);
  }
  for (int y = 0; y < 8; y++) dst[y] = rows[y];
}

static inline uint16_t MapEntry(uint16_t e, int layer) {
  uint16_t r = (e & 0x3ff) | ((e >> 4) & 0x0c00) | ((e << 2) & 0x7000);
  if (layer == 2) {
    // BG3: 2bpp palettes are GBA palettes 8-15; its tiles 0-255 only.
    r |= 0x8000;
    if ((e & 0x3ff) >= kSolidTile3) r = (r & ~0x3ff) | kBlankTile3;
  } else if ((e & 0x3ff) == kBlankTile12) {
    r = (r & ~0x3ff) | kBlankTile12;
  }
  return r;
}

// Converts 16 map entries starting at map word index i of a layer.
static void ConvertMapChunk(int layer, uint32_t i, const uint16_t *src) {
  volatile uint16_t *full = VRAM + SbFull(layer) * 0x400 + i;
  volatile uint16_t *hi = VRAM + SbHi(layer) * 0x400 + i;
  uint16_t blank = layer == 2 ? (0x8000 | kBlankTile3) : kBlankTile12;
  int row = (s_map_size[layer] & 2) ? ((i >> 10) & 1) * 32 + ((i >> 5) & 31)
                                    : (i >> 5) & 31;
  int delta = 0;
  for (int k = 0; k < 16; k++) {
    uint16_t e = src[k];
    uint16_t m = MapEntry(e, layer);
    full[k] = m;
    uint16_t h = (e & 0x2000) ? m : blank;
    delta += (h != blank) - (hi[k] != blank);
    hi[k] = h;
  }
  s_prio_rows[layer][row] += delta;
  s_prio_total[layer] += delta;
  // The title menu's cursor (BG1 tile 0x22e, right against the text on the
  // SNES) one column further left, for some room between it and the text.
  if (layer == 0 && s_tiles_squashed == kSquashTitle) {
    // (The entry to the left can be the previous chunk's last, not at a
    // map row's start.)
    for (int k = (i & 31) ? 0 : 1; k < 16; k++) {
      if ((src[k] & 0x3ff) == kTitleCursorTile && (src[k - 1] & 0x3ff) == kTitleBlankTile) {
        full[k - 1] = MapEntry(src[k], 0);
        full[k] = MapEntry(src[k - 1], 0);
      }
    }
  }
}

static inline bool InRange(uint32_t a, uint32_t base, uint32_t size) {
  return a - base < size;
}

static void ConvertChunk(uint32_t a) {
  const uint16_t *src = &g_snes_vram[a];
  if (InRange(a, s_bg12_base, 0x4000)) {
    uint32_t t = (a - s_bg12_base) >> 4;
    if (t != kBlankTile12)
      Convert4bpp((volatile uint32_t *)(VRAM + t * 16), src, s_tiles_squashed == kSquashFull);
  }
  if (InRange(a, s_bg3_base, 0x800)) {
    uint32_t t = (a - s_bg3_base) >> 3;
    for (int k = 0; k < 2; k++, t++)
      if (t < kSolidTile3)
        Convert2bpp((volatile uint32_t *)(VRAM + 0x4000 + t * 16), src + k * 8, false);
  }
  for (int n = 0; n < 2; n++) {
    if (InRange(a, s_obj_base[n], 0x1000)) {
      uint32_t t = ((a - s_obj_base[n]) >> 4) + n * 256;
      uint32_t g = (t >> 4) * 32 + (t & 15);
      volatile uint32_t *d = (volatile uint32_t *)(VRAM_OBJ + g * 16);
      Convert4bpp(d, src, false);
      for (int y = 0; y < 8; y++) d[y + 128] = d[y];  // the copy 16 tiles on
    }
  }
  for (int layer = 0; layer < 3; layer++) {
    uint32_t size = (s_map_size[layer] == 0) ? 0x400 : 0x800;
    if (InRange(a, s_map_base[layer], size))
      ConvertMapChunk(layer, a - s_map_base[layer], src);
  }
}

// Checks the VRAM layout registers; when they change, everything is
// converted again.
ROM_CODE static void CheckLayout(void) {
  SnesPpu *p = &g_snes->ppu;
  uint16_t bg12 = (p->bgnba[0] & 0xf) << 12;
  uint16_t bg2 = (p->bgnba[0] >> 4) << 12;
  uint16_t bg3 = (p->bgnba[1] & 0xf) << 12;
  uint16_t ob0 = (p->obsel & 7) << 13;
  uint16_t ob1 = (ob0 + (((p->obsel >> 3) & 3) + 1) * 0x1000) & 0x7fff;
  uint16_t mb[3];
  uint8_t ms[3];
  for (int i = 0; i < 3; i++) {
    mb[i] = (p->bgsc[i] & 0xfc) << 8;
    ms[i] = p->bgsc[i] & 3;
  }
  if (s_layout_valid && bg12 == s_bg12_base && bg3 == s_bg3_base &&
      ob0 == s_obj_base[0] && ob1 == s_obj_base[1] &&
      !memcmp(mb, s_map_base, sizeof(mb)) && !memcmp(ms, s_map_size, sizeof(ms)))
    return;
  if (bg2 != bg12)
    printf("render: BG1 tiles at %04x, BG2 at %04x: BG2 shows BG1's", bg12, bg2);
  s_bg12_base = bg12, s_bg3_base = bg3;
  s_obj_base[0] = ob0, s_obj_base[1] = ob1;
  memcpy(s_map_base, mb, sizeof(mb));
  memcpy(s_map_size, ms, sizeof(ms));
  s_layout_valid = true;
  memset(g_vram_dirty, 0xff, sizeof(g_vram_dirty));
  // The hi maps are rebuilt from blank, so the counts start from zero.
  memset(s_prio_rows, 0, sizeof(s_prio_rows));
  memset(s_prio_total, 0, sizeof(s_prio_total));
  for (int layer = 0; layer < 3; layer++) {
    uint16_t blank = layer == 2 ? (0x8000 | kBlankTile3) : kBlankTile12;
    volatile uint16_t *hi = VRAM + SbHi(layer) * 0x400;
    for (int i = 0; i < 0x800; i++) hi[i] = blank;
  }
}

__attribute__((noinline)) static void ConvertDirtyVram(void) {
  for (int w = 0; w < (int)(sizeof(g_vram_dirty) / 4); w++) {
    uint32_t bits = g_vram_dirty[w];
    if (!bits) continue;
    g_vram_dirty[w] = 0;
    for (int b = 0; bits; b++, bits >>= 1)
      if (bits & 1)
        ConvertChunk((w * 32 + b) * 16);
  }
}

ROM_CODE static void ConvertPalettes(void) {
  uint16_t d = g_cgram_dirty;
  g_cgram_dirty = 0;
  for (int p = 0; p < 16; p++) {
    if (!(d & (1 << p))) continue;
    const uint16_t *src = &g_snes_cgram[p * 16];
    volatile uint16_t *dst = (p < 8) ? PAL_BG + p * 16 : PAL_OBJ + (p - 8) * 16;
    for (int i = (p == 0); i < 16; i++) dst[i] = src[i];
    if (p < 2) {
      // BG3's 2bpp palettes: CGRAM 4q..4q+3 are GBA palette 8 + q.
      for (int q = p * 4; q < p * 4 + 4; q++)
        for (int i = 1; i < 4; i++)
          PAL_BG[(8 + q) * 16 + i] = g_snes_cgram[q * 4 + i];
    }
  }
}

// ---- layer planning ----

enum { kPlaneObj = 3 };
typedef struct Plane { uint8_t layer, prio; } Plane;

// Mode 1's planes, front to back, with BG3's priority bit (BGMODE bit 3)
// and without.
static const Plane kMode1Bg3Hi[10] = {
  {2, 1}, {3, 3}, {0, 1}, {1, 1}, {3, 2}, {0, 0}, {1, 0}, {3, 1}, {3, 0}, {2, 0},
};
static const Plane kMode1[10] = {
  {3, 3}, {0, 1}, {1, 1}, {3, 2}, {0, 0}, {1, 0}, {3, 1}, {2, 1}, {3, 0}, {2, 0},
};

typedef struct LayerPlan {
  int nbg;
  uint8_t bg_layer[4];   // SNES layer of each GBA BG
  bool bg_hi[4];         // shows the hi map (else the full map)
  uint8_t bg_prio[4];
  uint8_t obj_prio[4];   // GBA priority of each SNES sprite priority
} LayerPlan;

ROM_CODE static void PlanLayers(LayerPlan *lp, uint8_t main_mask, uint8_t sub_mask,
                                bool bg3hi, uint8_t obj_prios_used, int max_bg,
                                bool hud_front) {
  const Plane *order = bg3hi ? kMode1Bg3Hi : kMode1;
  // Which planes show: a layer's hi plane if it has priority tiles, its low
  // plane always; sprite planes if some sprite uses them.
  Plane planes[20];
  int n = 0;
  for (int pass = 0; pass < 2; pass++) {
    uint8_t mask = pass == 0 ? main_mask : (sub_mask & ~main_mask);
    for (int i = 0; i < 10; i++) {
      Plane pl = order[i];
      if (pl.layer == kPlaneObj) {
        if (pass == 0 && (mask & 0x10) && (obj_prios_used & (1 << pl.prio)))
          planes[n++] = pl;
      } else if (mask & (1 << pl.layer)) {
        if (pl.prio == 0 || s_prio_total[pl.layer])
          planes[n++] = pl;
      }
    }
  }
  // With the HUD overlaid on the level, BG3's priority tiles (the HUD) go in
  // front of everything: they're over rows of the level whose BG1 the SNES
  // shows elsewhere (a castle's ceiling would cover them).
  if (hud_front) {
    for (int i = 0; i < n; i++) {
      if (planes[i].layer == 2 && planes[i].prio) {
        Plane hp = planes[i];
        memmove(&planes[1], &planes[0], i * sizeof(Plane));
        planes[0] = hp;
        break;
      }
    }
  }
  // Count the GBA BGs: two for a layer with both planes. Too many: show a
  // layer whole at its priority tiles' place, BG3 first (in Mode 1 with its
  // priority bit, BG3 is the text and HUD layer, drawn over the rest), then
  // BG2, then BG1.
  int nbg = 0;
  for (int i = 0; i < n; i++) nbg += planes[i].layer != kPlaneObj;
  static const uint8_t kMergeOrder[2][3] = { { 2, 1, 0 }, { 1, 0, 2 } };
  for (int m = 0; m < 3 && nbg > max_bg; m++) {
    int layer = kMergeOrder[hud_front][m], hi = -1, lo = -1;
    for (int i = 0; i < n; i++) {
      if (planes[i].layer != layer) continue;
      if (planes[i].prio) hi = i; else lo = i;
    }
    if (hi < 0 || lo < 0) continue;
    memmove(&planes[lo], &planes[lo + 1], (n - lo - 1) * sizeof(Plane));
    n--, nbg--;
  }
  // Assign priorities front to back. A sprite plane behind a BG plane at the
  // current level needs the next level.
  int level = 0;
  bool bg_at_level = false;
  lp->nbg = 0;
  memset(lp->obj_prio, 3, sizeof(lp->obj_prio));
  for (int i = 0; i < n; i++) {
    Plane pl = planes[i];
    if (pl.layer == kPlaneObj) {
      if (bg_at_level && level < 3) level++, bg_at_level = false;
      lp->obj_prio[pl.prio] = level;
    } else if (lp->nbg < max_bg) {
      int k = lp->nbg++;
      lp->bg_layer[k] = pl.layer;
      lp->bg_hi[k] = false;
      // A hi plane whose layer also shows its low plane later uses the hi
      // map; the low plane (or a layer shown once) uses the full map.
      if (pl.prio) {
        for (int j = i + 1; j < n; j++)
          if (planes[j].layer == pl.layer) lp->bg_hi[k] = true;
      }
      lp->bg_prio[k] = level;
      bg_at_level = true;
    }
  }
  // Sprite priorities not shown take the level of the next lower one shown.
  for (int p = 3; p >= 0; p--) {
    if (!(obj_prios_used & (1 << p)))
      lp->obj_prio[p] = (p < 3) ? lp->obj_prio[p + 1] : 3;
  }
}

// ---- color math for the backdrop ----

static inline uint16_t AddColors(uint16_t a, uint16_t b, bool half, bool sub) {
  int r = a & 31, g = (a >> 5) & 31, bl = a >> 10;
  int r2 = b & 31, g2 = (b >> 5) & 31, b2 = b >> 10;
  if (sub) r -= r2, g -= g2, bl -= b2;
  else r += r2, g += g2, bl += b2;
  if (half) r >>= 1, g >>= 1, bl >>= 1;
  if (r < 0) r = 0; if (g < 0) g = 0; if (bl < 0) bl = 0;
  if (r > 31) r = 31; if (g > 31) g = 31; if (bl > 31) bl = 31;
  return r | g << 5 | bl << 10;
}

// Whether a line is in the color window, taking a window to cover its line
// when it spans at least half of it (the games' windows here are whole lines
// or nothing).
static bool ColorWindowCovers(const LineRegs *l, uint8_t wobjsel, uint8_t wobjlog) {
  bool w[2], en[2];
  for (int i = 0; i < 2; i++) {
    int left = l->wh[i * 2], right = l->wh[i * 2 + 1];
    en[i] = (wobjsel >> (5 + 2 * i)) & 1;
    w[i] = right - left >= 128;
    if ((wobjsel >> (4 + 2 * i)) & 1) w[i] = !w[i];
  }
  if (!en[0] && !en[1]) return false;
  if (!en[1]) return w[0];
  if (!en[0]) return w[1];
  switch ((wobjlog >> 2) & 3) {
  case 0: return w[0] || w[1];
  case 1: return w[0] && w[1];
  case 2: return w[0] != w[1];
  default: return w[0] == w[1];
  }
}

// Whether color math applies on a line, by CGWSEL's bits 4-5.
static bool MathOnLine(const LineRegs *l, const SnesPpu *p) {
  switch ((p->cgwsel >> 4) & 3) {
  case 0: return true;
  case 1: return ColorWindowCovers(l, p->wobjsel, p->wobjlog);
  case 2: return !ColorWindowCovers(l, p->wobjsel, p->wobjlog);
  default: return false;
  }
}

// ---- the squash ----
// GBA line g shows SNES row s_rowmap[g]; s_linemap[r] is the first GBA line
// showing row r or a later one (for sprites), negative above the first.
EWRAM_BSS static uint8_t s_rowmap[kGbaLines];
EWRAM_BSS static int16_t s_linemap[kSnesLines];

// The title's text bands: rows of BG1's map with the font's tiles (0x00-
// 0x2f, not the blank 0x24, 0x28). Its rows are kept whole; the title's
// other rows give up the 32 that must go, spread evenly.
ROM_CODE static bool TitleTextBand(int map_row) {
  const uint16_t *m = &g_snes_vram[s_map_base[0] + (map_row & 31) * 32];
  for (int c = 0; c < 32; c++) {
    uint16_t t = m[c] & 0x3ff;
    if (t < 0x30 && t != 0x24 && t != 0x28) return true;
  }
  return false;
}

ROM_CODE static void BuildSquashMap(int mode, int vofs3, int vofs1) {
  static int last_mode = -1, last3 = -1, last1 = -1;
  // The title's text is on BG1: its bands follow BG1's scroll.
  if (mode == kSquashTitle) vofs1 = LineScroll(kSnesLines / 2, 0) >> 16;
  static uint32_t last_bands;
  uint32_t bands = 0;
  if (mode == kSquashTitle) {
    for (int b = 0; b < 32; b++)
      if (TitleTextBand(b)) bands |= 1u << b;
  }
  if (mode == last_mode && vofs3 == last3 && vofs1 == last1 && bands == last_bands) return;
  last_mode = mode, last3 = vofs3, last1 = vofs1, last_bands = bands;
  int g = 0;
  if (mode == kSquashTitle) {
    // Rows 32-223 (no HUD): text rows kept, 32 of the others dropped.
    enum { kFirst = 32 };
    bool text[kSnesLines];
    int droppable = 0;
    for (int r = kFirst; r < kSnesLines; r++) {
      int band = ((vofs1 + r + 1) >> 3) & 31;
      text[r] = (bands >> band) & 1;
      droppable += !text[r];
    }
    int to_drop = (kSnesLines - kFirst) - kGbaLines, seen = 0;
    for (int r = 0; r < kSnesLines; r++) {
      if (r < kFirst) {
        s_linemap[r] = -1 - (kFirst - 1 - r);
        continue;
      }
      bool keep = true;
      if (!text[r] && droppable > 0) {
        // Drop this one if it takes the running count of drops up a step.
        keep = (seen + 1) * to_drop / droppable == seen * to_drop / droppable;
        seen++;
      }
      if (keep && g < kGbaLines) s_rowmap[g++] = r;
      s_linemap[r] = g < kGbaLines ? g : kGbaLines;
    }
  } else {
    // The whole screen: rows that aren't the first or last of a tile row of
    // the text layer kept (the font's glyphs have their fill in rows 1-6,
    // outlines in 0 and 7; BG3 has the HUD, rows 0-31, BG2 the rest), 168
    // of 224, and the first and last 4 of those dropped too.
    int kept = 0;
    for (int r = 0; r < kSnesLines; r++) {
      int phase = ((r < 32 ? vofs3 : vofs1) + r + 1) & 7;
      if (phase != 0 && phase != 7 && kept++ >= 4 && g < kGbaLines) s_rowmap[g++] = r;
      s_linemap[r] = g < kGbaLines ? g : kGbaLines;
    }
  }
  while (g < kGbaLines) s_rowmap[g] = s_rowmap[g - 1], g++;
}

// ---- sprites ----

static const uint8_t kObjSizes[8][2] = {
  {0, 1}, {0, 2}, {0, 3}, {1, 2}, {1, 3}, {2, 3}, {1, 2}, {1, 2},
};

__attribute__((noinline)) static uint8_t ObjPriosUsed(void) {
  uint8_t used = 0;
  for (int i = 0; i < 128; i++) {
    uint8_t y = g_snes_oam[i * 4 + 1];
    if (y >= 0xe0 && y < 0xf8) continue;  // off screen (most hidden ones)
    used |= 1 << ((g_snes_oam[i * 4 + 3] >> 4) & 3);
  }
  return used;
}

__attribute__((noinline)) static void BuildOam(uint16_t *out, const ViewMap *vm, const uint8_t *obj_prio) {
  SnesPpu *p = &g_snes->ppu;
  const uint8_t *sizes = kObjSizes[p->obsel >> 5];
  static const uint8_t kPix[4] = { 8, 16, 32, 64 };
  // Squashed, each group of sprites drawn together (consecutive in OAM and
  // touching: Mario's eight tiles, a Koopa's) moves as one, by its bottom
  // row's place in the row map, so it keeps its shape and its feet stay on
  // the ground. s_shift is each sprite's move: SNES row minus GBA line.
  EWRAM_BSS static int16_t s_shift[128];
  if (vm->squash) {
    int start = 0;
    for (int i = 0; i <= 128; i++) {
      bool joins = false;
      if (i > 0 && i < 128) {
        const uint8_t *a = &g_snes_oam[(i - 1) * 4], *b = &g_snes_oam[i * 4];
        int dx = a[0] - b[0], dy = a[1] - b[1];
        joins = dx >= -16 && dx <= 16 && dy >= -16 && dy <= 16;
      }
      if (i == 128 || (i > 0 && !joins)) {
        int bottom = 0;
        for (int k = start; k < i; k++) {
          uint8_t hb = g_snes_oam[0x200 + (k >> 2)] >> ((k & 3) * 2);
          int b = g_snes_oam[k * 4 + 1] + kPix[sizes[(hb >> 1) & 1]] - 1;
          if (g_snes_oam[k * 4 + 1] < kSnesLines && b > bottom) bottom = b;
        }
        if (bottom >= kSnesLines) bottom = kSnesLines - 1;
        int shift = bottom - s_linemap[bottom];
        for (int k = start; k < i; k++) s_shift[k] = shift;
        start = i;
      }
    }
  }
  for (int i = 0; i < 128; i++) {
    const uint8_t *o = &g_snes_oam[i * 4];
    uint8_t hb = g_snes_oam[0x200 + (i >> 2)] >> ((i & 3) * 2);
    int size = sizes[(hb >> 1) & 1];
    int h = kPix[size];
    int x = o[0] | (hb & 1) << 8;
    if (x >= 256) x -= 512;
    int y = o[1];
    if (y + h > 256) y -= 256;  // wraps to the top
    // Which region of the view it falls in, by its top line.
    int gy = y - vm->offset;
    if (vm->squash)
      gy = y - s_shift[i];
    else if (vm->split && !vm->hud_overlay && y < vm->split + vm->top_offset)
      gy = y - vm->top_offset;
    int gx = x - vm->xoffset;
    uint16_t *d = &out[i * 4];
    if (gy >= kGbaLines || gy + h <= 0 || gx >= 240 || gx + kPix[size] <= 0 || y >= 224) {
      d[0] = 0x200, d[1] = 0, d[2] = 0;
      continue;
    }
    uint32_t t = o[2] | (o[3] & 1) << 8;
    uint32_t g = (t >> 4) * 32 + (t & 15);
    d[0] = (gy & 0xff);
    d[1] = (gx & 0x1ff) | ((o[3] & 0x40) << 6) | ((o[3] & 0x80) << 6) | size << 14;
    d[2] = g | obj_prio[(o[3] >> 4) & 3] << 10 | ((o[3] >> 1) & 7) << 12;
  }
}

// ---- the frame ----

ROM_CODE void RenderInit(void) {
  s_front = NULL;
  InitSpread();
  // Transparent tiles.
  memset((void *)(VRAM + kBlankTile12 * 16), 0, 32);
  memset((void *)(VRAM + 0x4000 + kBlankTile3 * 16), 0, 32);
  for (int i = 0; i < 8; i++)
    ((volatile uint32_t *)(VRAM + 0x4000 + kSolidTile3 * 16))[i] = 0xffffffff;
  for (int i = 0; i < 0x400; i++)
    VRAM[kTintSb * 0x400 + i] = 0xf000 | kSolidTile3;
  s_layout_valid = false;
}

void RenderBuildFrame(const ViewMap *vm) {
  int b = s_back;
  FrameRegs *fr = &s_regs[b];
  LineTable *lt = &s_tables[b];
  SnesPpu *p = &g_snes->ppu;

  fr->blank = (p->inidisp & 0x80) != 0;
  fr->squash = vm->squash;
  if (vm->squash)
    BuildSquashMap(vm->squash, LineScroll(0, 2) >> 16, LineScroll(kSnesLines - 1, 1) >> 16);
  uint8_t tm = g_line_states[g_line_state[0]].tm, ts = g_line_states[g_line_state[0]].ts;
  bool sub_add = (p->cgwsel & 2) && (p->cgadsub & 0x20) && !(p->cgadsub & 0x1f);
  uint8_t sub_mask = sub_add ? ts : 0;

  uint8_t obj_used = ObjPriosUsed();
  int bright = p->inidisp & 0xf;
  uint8_t math = p->cgadsub;

  // The fixed color added to the main screen's layers (the water levels'
  // tint): a layer of solid color on top, its color per line the fixed
  // color, or black where math is off, blended with EVA = EVB = 16: the
  // GBA's sum, clamped at 31, is the SNES's. BG3, if not in the math, goes
  // above it (the HUD); the rest is planned below it, one level down.
  bool tint = !(p->cgwsel & 2) && (math & 0x13) && !(math & 0xc4) && bright == 15;
  LayerPlan lp;
  int k0 = 0;
  if (tint) {
    k0 = (tm & 0x04) ? 2 : 1;
    PlanLayers(&lp, tm & ~0x04, 0, (p->bgmode & 8) != 0, obj_used, 4 - k0, false);
  }
  if (!tint) {
    k0 = 0;
    PlanLayers(&lp, tm, sub_mask, (p->bgmode & 8) != 0, obj_used, 4, vm->hud_overlay);
  }
  fr->tint = tint;

  uint16_t dispcnt = DCNT_MODE0 | DCNT_OBJ_2D | DCNT_OBJ;
  uint8_t mosaic_layers = p->mosaic & 0xf;
  uint8_t layer_of[4] = { 0, 0, 0, 0 };
  int tint_bg = -1;
  if (tint) {
    if (tm & 0x04) {
      int size = s_map_size[2] == 3 ? 1 : s_map_size[2];
      fr->bgcnt[0] = 0 | 2 << 2 | ((mosaic_layers >> 2) & 1) << 6 | SbFull(2) << 8 | size << 14;
      layer_of[0] = 2;
      dispcnt |= DCNT_BG0;
    }
    tint_bg = k0 - 1;
    fr->bgcnt[tint_bg] = 0 | 2 << 2 | kTintSb << 8;
    dispcnt |= DCNT_BG0 << tint_bg;
    layer_of[tint_bg] = 0;
    for (int p2 = 0; p2 < 4; p2++)
      if (lp.obj_prio[p2] < 3) lp.obj_prio[p2]++;
  }
  for (int k = 0; k < lp.nbg; k++) {
    int layer = lp.bg_layer[k];
    int sb = lp.bg_hi[k] ? SbHi(layer) : SbFull(layer);
    int size = s_map_size[layer] == 3 ? 1 : s_map_size[layer];
    int prio = lp.bg_prio[k] + (tint ? 1 : 0);
    if (prio > 3) prio = 3;
    fr->bgcnt[k + k0] = prio | (layer == 2 ? 2 : 0) << 2 |
                        ((mosaic_layers >> layer) & 1) << 6 | sb << 8 | size << 14;
    layer_of[k + k0] = layer;
    dispcnt |= DCNT_BG0 << (k + k0);
  }
  if (!(tm & 0x10)) dispcnt &= ~DCNT_OBJ;
  fr->dispcnt = dispcnt;
  int msz = p->mosaic >> 4;
  fr->mosaic = msz | msz << 4 | msz << 8 | msz << 12;

  fr->bldcnt = 0, fr->bldalpha = 0, fr->bldy = 0;
  if (tint) {
    uint16_t second = 0;
    for (int k = k0; k < k0 + lp.nbg; k++)
      if (math & (1 << layer_of[k])) second |= 1 << (8 + k);
    if (math & 0x10) second |= 1 << 12;
    if (math & 0x20) second |= 1 << 13;
    fr->bldcnt = (1 << tint_bg) | 1 << 6 | second;
    fr->bldalpha = 16 | 16 << 8;
  } else if (bright < 15) {
    // Brightness: the GBA darkens toward black with BLDY.
    fr->bldcnt = 0x3f | 3 << 6;
    fr->bldy = (16 * (15 - bright) + 7) / 15;
  }

  // Per-line scrolling and backdrop.
  bool bd_math = (p->cgadsub & 0x20) != 0;
  bool half = (p->cgadsub & 0x40) != 0, subtract = (p->cgadsub & 0x80) != 0;
  uint16_t cg0 = g_snes_cgram[0];
  bool per_line_bd = false;
  // Each GBA line's SNES row (and BG3's, which with the HUD overlaid shows
  // the HUD's rows at the top).
  static uint8_t row_of[kGbaLines + 1], row3_of[kGbaLines + 1];
  for (int g = 0; g <= kGbaLines; g++) {
    int gl = g < kGbaLines ? g : kGbaLines - 1;
    bool top = vm->split && gl < vm->split;
    int r = vm->squash ? s_rowmap[gl]
                       : gl + ((top && !vm->hud_overlay) ? vm->top_offset : vm->offset);
    if (r < 0) r = 0;
    if (r >= kSnesLines) r = kSnesLines - 1;
    int rh = r;
    if (top && vm->hud_overlay) {
      rh = gl + vm->top_offset;
      if (rh < 0) rh = 0;
    }
    row_of[g] = r, row3_of[g] = rh;
  }
  // The scroll registers, a layer at a time: hofs + xoffset | vofs + 1 +
  // row - line (line kGbaLines repeats the last one).
  int nused = k0 + lp.nbg;
  for (int k = 0; k < 4; k++) {
    int layer = layer_of[k];
    const uint8_t *rows = layer == 2 ? row3_of : row_of;
    uint32_t *col = (uint32_t *)lt->scroll + k;
    if (k >= nused || (tint && k == tint_bg)) {
      // Not shown, or the tint (one tile everywhere): no scroll.
      for (int g = 0; g <= kGbaLines; g++) col[g * 4] = 0;
      continue;
    }
    const uint32_t *states = &g_scroll_states[0][layer];
    const uint16_t *bh = (g_scroll_baked & (1 << layer)) ? g_scroll_bake[layer] : NULL;
    const uint16_t *bv = (g_scroll_baked & (16 << layer)) ? g_scroll_bake[4 + layer] : NULL;
    uint32_t changes = (kLineChanged_Hofs0 | kLineChanged_Vofs0) << layer;
    if (!bh && !bv && !(g_line_changed & changes)) {
      // The same scroll on every line: only the row-line distance varies.
      uint32_t v = states[0];
      uint32_t h = ((v & 0x3ff) + vm->xoffset) & 0xffff, vv = (v >> 16) + 1;
      for (int g = 0; g <= kGbaLines; g++) {
        int gl = g < kGbaLines ? g : kGbaLines - 1;
        col[g * 4] = h | ((vv + rows[g] - gl) << 16);
      }
      continue;
    }
    if (!bh && !bv) {
      // A few scroll records (the HUD's split): lines with the previous
      // line's record and row-line distance get its entry.
      uint32_t last_key = 0xffffffff, e = 0;
      for (int g = 0; g <= kGbaLines; g++) {
        int gl = g < kGbaLines ? g : kGbaLines - 1;
        int r = rows[g];
        uint32_t idx = g_line_scroll_state[r];
        uint32_t key = idx | (uint32_t)(r - gl) << 8;
        if (key != last_key) {
          last_key = key;
          uint32_t v = states[idx * 4];
          e = (((v & 0x3ff) + vm->xoffset) & 0xffff) | (((v >> 16) + 1 + r - gl) << 16);
        }
        col[g * 4] = e;
      }
      continue;
    }
    for (int g = 0; g <= kGbaLines; g++) {
      int gl = g < kGbaLines ? g : kGbaLines - 1;
      int r = rows[g];
      uint32_t v = states[g_line_scroll_state[r] * 4];
      uint32_t h = bh ? bh[r] : (v & 0x3ff), vv = bv ? bv[r] : (v >> 16);
      col[g * 4] = ((h + vm->xoffset) & 0xffff) | ((vv + 1 + r - gl) << 16);
    }
  }
  // The backdrop's (or the tint's) color, from each line's state.
  uint16_t bd = cg0, bd0 = 0;
  int last_st = -1;
  for (int g = 0; g <= kGbaLines; g++) {
    int st = g_line_state[row_of[g]];
    if (st != last_st) {
      last_st = st;
      const LineRegs *lr = &g_line_states[st];
      if (tint)
        bd = MathOnLine(lr, p) ? lr->fixed_color : 0;
      else if (bd_math)
        bd = AddColors(cg0, lr->fixed_color, half, subtract);
    }
    if (g == 0) bd0 = bd;
    lt->backdrop[g] = bd;
    if (bd != bd0) per_line_bd = true;
  }
  fr->backdrop_per_line = per_line_bd;

  BuildOam(s_oam[b], vm, lp.obj_prio);
  s_oam_ready[b] = true;
}

ROM_CODE void RenderPresent(void) {
  int b = s_back;
  s_back ^= 1;
  FrameRegs *fr = &s_regs[b];
  LineTable *lt = &s_tables[b];

  CheckLayout();
  if (fr->squash != s_tiles_squashed) {
    // Only BG1/BG2's tiles are converted differently.
    s_tiles_squashed = fr->squash;
    for (uint32_t a = s_bg12_base; a < (uint32_t)s_bg12_base + 0x4000; a += 16)
      g_vram_dirty[(a & 0x7fff) >> 9] |= 1u << ((a >> 4) & 31);
    // And BG1's map (the title's cursor).
    for (uint32_t a = s_map_base[0]; a < (uint32_t)s_map_base[0] + 0x800; a += 16)
      g_vram_dirty[(a & 0x7fff) >> 9] |= 1u << ((a >> 4) & 31);
  }
  ConvertDirtyVram();
  ConvertPalettes();

  // If converting ran past the VBlank (a level's load), the registers wait
  // for the next one: changed in mid-picture they'd tear it.
  while (REG_VCOUNT < 160) {}

  s_front = NULL;
  REG_DMA_CNT(0) = 0;
  REG_DMA_CNT(3) = 0;
  if (s_oam_ready[b]) {
    DmaCopy32(3, (void *)OAM, s_oam[b], 1024);
    s_oam_ready[b] = false;
  }
  if (fr->blank) {
    REG_DISPCNT = 0;
    REG_BLDCNT = 0x20 | 3 << 6;   // the backdrop, darkened to black
    REG_BLDY = 16;
    PAL_BG[0] = 0;
    return;
  }
  REG_DISPCNT = fr->dispcnt;
  for (int k = 0; k < 4; k++) REG_BGCNT(k) = fr->bgcnt[k];
  REG_BLDCNT = fr->bldcnt;
  REG_BLDALPHA = fr->bldalpha;
  REG_BLDY = fr->bldy;
  REG_MOSAIC = fr->mosaic;

  // The per-line color is the backdrop's, or with the tint, the tint's.
  if (fr->tint) PAL_BG[0] = g_snes_cgram[0];
  s_front_color = fr->tint ? &PAL_BG[15 * 16 + 15] : &PAL_BG[0];
  s_front_per_line = fr->backdrop_per_line;
  s_front = lt;
  RenderVBlank();
}

// Starts the picture's per-line registers from the top: line 0's written
// now, HBlank DMA bringing each next line's from entry 1 on. Done when a
// frame is presented and again at every VBlank (from its interrupt): a game
// frame that takes more than one VBlank is shown again, and the DMAs, left
// alone, would read on past the tables' ends.
void RenderVBlank(void) {
  const LineTable *lt = s_front;
  if (!lt) return;
  REG_DMA_CNT(0) = 0;
  volatile uint32_t *scroll_regs = (volatile uint32_t *)0x04000010;
  const uint32_t *line0 = (const uint32_t *)lt->scroll[0];
  for (int i = 0; i < 4; i++) scroll_regs[i] = line0[i];
  REG_DMA_SAD(0) = (uint32_t)lt->scroll[1];
  REG_DMA_DAD(0) = 0x04000010;
  REG_DMA_CNT(0) = 4 | DMA_32 | DMA_DST_RELOAD | DMA_REPEAT | DMA_AT_HBLANK | DMA_ENABLE;
  *s_front_color = lt->backdrop[0];
  if (s_front_per_line) {
    REG_DMA_CNT(3) = 0;
    REG_DMA_SAD(3) = (uint32_t)&lt->backdrop[1];
    REG_DMA_DAD(3) = (uint32_t)s_front_color;
    REG_DMA_CNT(3) = 1 | DMA_16 | DMA_DST_FIXED | DMA_REPEAT | DMA_AT_HBLANK | DMA_ENABLE;
  }
}

// Stops the per-line registers (leaving for the game select).
ROM_CODE void RenderStop(void) {
  s_front = NULL;
  REG_DMA_CNT(0) = 0;
  REG_DMA_CNT(3) = 0;
}
