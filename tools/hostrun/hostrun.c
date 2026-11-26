// hostrun: the PC reference for the GBA port.
//
// Runs snesrev's C version of SMB1 or The Lost Levels (the same game code the
// GBA build compiles) on the PC, with snesrev's SNES PPU drawing each frame,
// and optionally the original 65816 code running alongside on an emulated
// SNES, compared every frame ("-m both"). It's the ground truth for the GBA
// renderer (what a SNES shows from the same state) and for the game logic
// (the per-frame RAM hash, -H, diffed against the GBA build's).
//
// Usage: hostrun ROM [-n FRAMES] [-k SCRIPT] [-s SHOTS] [-d DUMPS] [-o DIR]
//                    [-m mine|both|theirs] [-H] [-j JOYLOG]
//   ROM     smb1.sfc or smbll.sfc (made by tools/make_cartridge.py --extract)
//   SCRIPT  words "KEYS*N" (N frames, default 1): KEYS is '-' or keys joined
//           by '+': up down left right a b l r start select, named as on the
//           GBA (a = SNES B, b = SNES Y), and x y for the SNES's A and X.
//           "( ... )xN" repeats a group.
//   SHOTS   frame numbers: "100,200,300-400/10" (PNG to DIR/fNNNNNN.png)
//   DUMPS   frame numbers to dump state to DIR/fNNNNNN.dump (see WriteDump)
//   -w      FRAME:ADDR=VAL[,ADDR=VAL...]: pokes the game's RAM (hex ADDR,
//           hex VAL) before game frame FRAME; repeatable.
//   -a      writes the sound (stereo s16, 32000 Hz) to a raw file.
//   JOYLOG  a file with one hex SNES joypad word per frame (from the GBA
//           build's trace); replaces SCRIPT.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <png.h>

#include "src/types.h"
#include "src/common_rtl.h"
#include "src/common_cpu_infra.h"
#include "src/smw_spc_player.h"
#include "src/snes/snes.h"
#include "src/snes/ppu.h"
#include "src/snes/dma.h"
#include "assets/smw_assets.h"
#include "src/config.h"
#include "src/util.h"

// Symbols the upstream main.c and config.c provide.
bool g_debug_flag;
bool g_want_dump_memmap_flags;
bool g_new_ppu = true;
bool g_other_image = true;
int g_got_mismatch_count;
SpcPlayer *g_spc_player;
const uint8 *g_asset_ptrs[kNumberOfAssets];
uint32 g_asset_sizes[kNumberOfAssets];
Config g_config;
extern uint8 g_runmode;

void NORETURN Die(const char *error) {
  fprintf(stderr, "Error: %s\n", error);
  exit(1);
}
void Warning(const char *error) { fprintf(stderr, "Warning: %s\n", error); }
void RtlApuLock(void) {}
void RtlApuUnlock(void) {}
MemBlk FindInAssetArray(int asset, int idx) {
  return FindIndexInMemblk((MemBlk) { g_asset_ptrs[asset], g_asset_sizes[asset] }, idx);
}
const uint8 *FindPtrInAsset(int asset, uint32 addr) {
  return FindAddrInMemblk((MemBlk) { g_asset_ptrs[asset], g_asset_sizes[asset] }, addr);
}
int FindCmdForSdlKey(SDL_Keycode code, SDL_Keymod mod) { return 0; }

static uint8 g_pixels[256 * 4 * 240], g_my_pixels[256 * 4 * 240];

// ---- input script ----
typedef struct { uint16 keys; int frames; } Step;
static Step *g_steps;
static int g_nsteps, g_cap;

static void AddStep(uint16 keys, int frames) {
  if (g_nsteps == g_cap)
    g_steps = realloc(g_steps, sizeof(Step) * (g_cap = g_cap * 2 + 16));
  g_steps[g_nsteps++] = (Step){ keys, frames };
}

// Bits of the 12-bit state RtlRunFrame takes (bit i is JOY bit 15 - i).
// Keys are named as on the GBA, mapped as the GBA build maps them (gba/main.c):
// A is the SNES's B (jump), B is its Y (run); "x" and "y" are the SNES's A and X.
static int KeyBit(const char *s, size_t n) {
  static const char *const kNames[12] = { "a", "b", "select", "start", "up", "down",
                                          "left", "right", "x", "y", "l", "r" };
  for (int i = 0; i < 12; i++)
    if (strlen(kNames[i]) == n && memcmp(kNames[i], s, n) == 0)
      return 1 << i;
  fprintf(stderr, "bad key '%.*s'\n", (int)n, s);
  exit(1);
}

static const char *ParseScript(const char *p, int depth) {
  for (;;) {
    while (*p == ' ' || *p == '\n' || *p == '\t') p++;
    if (!*p) return p;
    if (*p == ')') {
      if (!depth) { fprintf(stderr, "unbalanced )\n"); exit(1); }
      return p;
    }
    if (*p == '(') {
      int start = g_nsteps;
      p = ParseScript(p + 1, depth + 1);
      if (*p != ')') { fprintf(stderr, "missing )\n"); exit(1); }
      p++;
      int times = 1;
      if (*p == 'x') times = strtol(p + 1, (char **)&p, 10);
      int end = g_nsteps;
      for (int t = 1; t < times; t++)
        for (int i = start; i < end; i++) AddStep(g_steps[i].keys, g_steps[i].frames);
      continue;
    }
    const char *w = p;
    while (*p && *p != ' ' && *p != '\n' && *p != '\t' && *p != '*' && *p != ')') p++;
    uint16 keys = 0;
    if (!(p - w == 1 && *w == '-')) {
      for (const char *q = w; q < p;) {
        const char *e = q;
        while (e < p && *e != '+') e++;
        keys |= KeyBit(q, e - q);
        q = e + (e < p);
      }
    }
    int frames = 1;
    if (*p == '*') frames = strtol(p + 1, (char **)&p, 10);
    AddStep(keys, frames);
  }
}

// ---- frame lists ----
typedef struct { int *v; int n; } FrameList;
static FrameList ParseFrames(const char *s) {
  FrameList fl = { 0 };
  while (s && *s) {
    char *e;
    int a = strtol(s, &e, 10), b = a, step = 1;
    if (*e == '-') b = strtol(e + 1, &e, 10);
    if (*e == '/') step = strtol(e + 1, &e, 10);
    for (int i = a; i <= b; i += step) {
      fl.v = realloc(fl.v, sizeof(int) * (fl.n + 1));
      fl.v[fl.n++] = i;
    }
    s = (*e == ',') ? e + 1 : e;
    if (*e && *e != ',') break;
  }
  return fl;
}
static bool InList(FrameList *fl, int f) {
  for (int i = 0; i < fl->n; i++)
    if (fl->v[i] == f) return true;
  return false;
}

static void WritePng(const char *path, const uint8 *bgra, int w, int h, int pitch) {
  FILE *f = fopen(path, "wb");
  if (!f) { perror(path); return; }
  png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  png_infop info = png_create_info_struct(png);
  png_init_io(png, f);
  png_set_IHDR(png, info, w, h, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
               PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
  png_write_info(png, info);
  uint8 *row = malloc(w * 3);
  for (int y = 0; y < h; y++) {
    const uint8 *s = bgra + y * pitch;
    for (int x = 0; x < w; x++) {
      row[x * 3 + 0] = s[x * 4 + 2];
      row[x * 3 + 1] = s[x * 4 + 1];
      row[x * 3 + 2] = s[x * 4 + 0];
    }
    png_write_row(png, row);
  }
  free(row);
  png_write_end(png, NULL);
  png_destroy_write_struct(&png, &info);
  fclose(f);
}

// A state dump: the same layout tools/snesref and the GBA dumps use.
//   "SMASDUMP" u32 version=1
//   ram[0x20000] vram[0x10000] cgram[0x200] oam[0x220]
//   ppu regs: the 0x40 bytes the PPU snapshots (inidisp..)
static void WriteDump(const char *path, Ppu *ppu) {
  FILE *f = fopen(path, "wb");
  if (!f) { perror(path); return; }
  uint32 ver = 1;
  fwrite("SMASDUMP", 1, 8, f);
  fwrite(&ver, 4, 1, f);
  fwrite(g_snes->ram, 1, 0x20000, f);
  fwrite(ppu->vram, 2, 0x8000, f);
  fwrite(ppu->cgram, 2, 0x100, f);
  fwrite(ppu->oam, 2, 0x110, f);
  fwrite(&ppu->inidisp, 1, PPU_SAVESTATE_REGS_SIZE, f);
  fclose(f);
}

static uint32 Fnv1a(const uint8 *p, size_t n) {
  uint32 h = 2166136261u;
  while (n--) h = (h ^ *p++) * 16777619u;
  return h;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: hostrun ROM [-n FRAMES] [-k SCRIPT] [-s SHOTS] [-d DUMPS] [-o DIR] [-m mine|both|theirs] [-H] [-j JOYLOG]\n");
    return 1;
  }
  const char *rom_path = argv[1];
  int nframes = 600;
  const char *script = "", *outdir = ".", *joylog = NULL, *audio_path = NULL;
  FrameList shots = { 0 }, dumps = { 0 };
  bool print_hash = false;
  struct { int frame; char *spec; } pokes[16];
  int npokes = 0;
  g_runmode = 1;  // RM_MINE
  for (int i = 2; i < argc; i++) {
    const char *a = argv[i], *v = (i + 1 < argc) ? argv[i + 1] : "";
    if (!strcmp(a, "-n")) nframes = atoi(v), i++;
    else if (!strcmp(a, "-k")) script = v, i++;
    else if (!strcmp(a, "-s")) shots = ParseFrames(v), i++;
    else if (!strcmp(a, "-d")) dumps = ParseFrames(v), i++;
    else if (!strcmp(a, "-o")) outdir = v, i++;
    else if (!strcmp(a, "-j")) joylog = v, i++;
    else if (!strcmp(a, "-a")) audio_path = v, i++;
    else if (!strcmp(a, "-w") && npokes < 16) {
      pokes[npokes].frame = atoi(v);
      pokes[npokes].spec = strchr(v, ':') ? strchr(v, ':') + 1 : "";
      npokes++, i++;
    }
    else if (!strcmp(a, "-H")) print_hash = true;
    else if (!strcmp(a, "-m")) {
      g_runmode = !strcmp(v, "both") ? 0 : !strcmp(v, "theirs") ? 2 : 1;
      i++;
    } else { fprintf(stderr, "bad arg %s\n", a); return 1; }
  }
  ParseScript(script, 0);

  size_t rom_size;
  uint8 *rom = ReadWholeFile(rom_path, &rom_size);
  if (!rom) Die("can't read rom");
  // RM_MINE skips loading the ROM in SnesInit; load it anyway, the C code reads it.
  uint8 saved_mode = g_runmode;
  if (g_runmode == 1) g_runmode = 0;
  Snes *snes = SnesInit(rom, (int)rom_size);
  g_runmode = saved_mode;
  if (!snes) Die("SnesInit failed");
  g_spc_player = SmasSpcPlayer_Create();
  g_spc_player->initialize(g_spc_player);
  PpuBeginDrawing(g_snes->ppu, g_pixels, 256 * 4, 0);
  PpuBeginDrawing(g_my_ppu, g_my_pixels, 256 * 4, 0);

  FILE *jf = joylog ? fopen(joylog, "r") : NULL;
  FILE *af = audio_path ? fopen(audio_path, "wb") : NULL;
  static int16 audio_buf[534 * 2];
  if (joylog && !jf) Die("can't open joylog");

  int step = 0, step_left = g_nsteps ? g_steps[0].frames : 0;
  char path[512];
  for (int frame = 0; frame < nframes; frame++) {
    uint32 keys = 0;
    if (jf) {
      unsigned v;
      if (fscanf(jf, "%x", &v) != 1) break;
      keys = v;
    } else if (step < g_nsteps) {
      keys = g_steps[step].keys;
      if (--step_left <= 0 && ++step < g_nsteps)
        step_left = g_steps[step].frames;
    }
    for (int p = 0; p < npokes; p++) {
      if (pokes[p].frame != frame) continue;
      for (char *q = pokes[p].spec; *q;) {
        unsigned addr = strtoul(q, &q, 16), val = 0;
        if (*q == '=') val = strtoul(q + 1, &q, 16);
        g_ram[addr & 0x1ffff] = val;
        if (*q == ',') q++; else break;
      }
    }
    RtlRunFrame(keys);
    if (af) {
      RtlRenderAudio(audio_buf, 534, 2);
      fwrite(audio_buf, 4, 534, af);
    }
    Ppu *ppu = g_other_image ? g_my_ppu : g_snes->ppu;
    if (g_runmode == 2) ppu = g_snes->ppu;
    if (print_hash)
      printf("frame %d keys %03x ram %08x\n", frame, keys, Fnv1a(g_snes->ram, 0x2000));
    if (InList(&shots, frame)) {
      snprintf(path, sizeof(path), "%s/f%06d.png", outdir, frame);
      WritePng(path, (ppu == g_my_ppu) ? g_my_pixels : g_pixels, 256, 224, 256 * 4);
    }
    if (InList(&dumps, frame)) {
      snprintf(path, sizeof(path), "%s/f%06d.dump", outdir, frame);
      WriteDump(path, ppu);
    }
  }
  if (af) fclose(af);
  return 0;
}
