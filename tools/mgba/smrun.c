// smrun: runs the cartridge headless on libmgba, for tests.
//
//   smrun CART [-n FRAMES] [-k SCRIPT] [-s SHOTS] [-o DIR] [-q]
//              [-m FRAMES:ADDR:LEN ...] [-a AUDIO.raw] [-S STATE] [-W STATE_OUT@FRAME]
//
//   SCRIPT  words "KEYS*N" (N frames, default 1): KEYS is '-' or GBA keys
//           joined by '+': a b select start right left up down r l.
//           "( ... )xN" repeats a group.
//   SHOTS   frame numbers "100,200,300-400/10": PNG to DIR/gNNNNNN.png
//   -m      dumps LEN bytes of the GBA's memory at ADDR (hex) at the frames
//           to DIR/mNNNNNN_ADDR.bin; repeatable.
//   -a      records the sound (stereo s16 at 32768 Hz) to a raw file.
//   -f      ADDR (hex): the game's frame counter (g_game_frame). With it, the
//           script and the shots count game frames, as tools/hostrun does:
//           shot N is the picture of game frame N.
//   -w      FRAME:ADDR=VAL[,...] (hex ADDR and VAL): pokes the game's RAM
//           (g_ram, at the address given with -r) before game frame FRAME
//           starts; needs -f.
//   -K      a script for the GBA frames before the game starts (the game
//           select), by GBA frame; with -f, -k then starts with the game.
//   -S      keeps the cartridge's SRAM in a file next to it (CART.sav),
//           loaded at the start and written at the end.
//   -p      FROM-TO: profiles those frames by stepping the CPU, and writes
//           "address cycles" lines to DIR/profile.txt (tools/profile.py
//           sums them by function).
// The game's debug log (printf in the GBA build) is printed with the frame
// number; -q prints only the log.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <png.h>

#include <mgba/flags.h>
#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba/core/config.h>
#include <mgba-util/vfs.h>
#include <mgba-util/audio-buffer.h>
#include <mgba/internal/arm/arm.h>

static int g_frame;
static bool g_quiet;

static void LogCb(struct mLogger *logger, int category, enum mLogLevel level,
                  const char *format, va_list args) {
  const char *cat = mLogCategoryName(category);
  bool is_debug = cat && strcmp(cat, "GBA Debug") == 0;
  if (!is_debug && (level & (mLOG_DEBUG | mLOG_STUB | mLOG_INFO)))
    return;
  if (!is_debug && g_quiet) return;
  printf("[%d] ", g_frame);
  if (!is_debug) printf("%s: ", cat ? cat : "?");
  vprintf(format, args);
  printf("\n");
}
static struct mLogger g_logger = { .log = LogCb };

// ---- scripts and frame lists (as in tools/hostrun) ----
typedef struct { uint32_t keys; int frames; } Step;
static Step *g_steps;
static int g_nsteps, g_cap;
static void AddStep(uint32_t keys, int frames) {
  if (g_nsteps == g_cap) g_steps = realloc(g_steps, sizeof(Step) * (g_cap = g_cap * 2 + 16));
  g_steps[g_nsteps++] = (Step){ keys, frames };
}
static uint32_t KeyBit(const char *s, size_t n) {
  static const char *const kNames[10] = { "a", "b", "select", "start", "right",
                                          "left", "up", "down", "r", "l" };
  for (int i = 0; i < 10; i++)
    if (strlen(kNames[i]) == n && memcmp(kNames[i], s, n) == 0) return 1u << i;
  fprintf(stderr, "bad key '%.*s'\n", (int)n, s);
  exit(1);
}
static const char *ParseScript(const char *p, int depth) {
  for (;;) {
    while (*p == ' ' || *p == '\n' || *p == '\t') p++;
    if (!*p || *p == ')') return p;
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
    uint32_t keys = 0;
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
typedef struct { int *v; int n; } FrameList;
static void ParseFramesInto(FrameList *fl, const char *s) {
  while (s && *s) {
    char *e;
    int a = strtol(s, &e, 10), b = a, step = 1;
    if (*e == '-') b = strtol(e + 1, &e, 10);
    if (*e == '/') step = strtol(e + 1, &e, 10);
    for (int i = a; i <= b; i += step) {
      fl->v = realloc(fl->v, sizeof(int) * (fl->n + 1));
      fl->v[fl->n++] = i;
    }
    if (*e != ',') break;
    s = e + 1;
  }
}
static bool InList(const FrameList *fl, int f) {
  for (int i = 0; i < fl->n; i++)
    if (fl->v[i] == f) return true;
  return false;
}

static void WritePng(const char *path, const uint32_t *px, int w, int h) {
  FILE *f = fopen(path, "wb");
  if (!f) { perror(path); return; }
  png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  png_infop info = png_create_info_struct(png);
  png_init_io(png, f);
  png_set_IHDR(png, info, w, h, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
               PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
  png_write_info(png, info);
  uint8_t *row = malloc(w * 3);
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      uint32_t c = px[y * w + x];  // mGBA's XBGR8
      row[x * 3 + 0] = c & 0xff;
      row[x * 3 + 1] = (c >> 8) & 0xff;
      row[x * 3 + 2] = (c >> 16) & 0xff;
    }
    png_write_row(png, row);
  }
  free(row);
  png_write_end(png, NULL);
  png_destroy_write_struct(&png, &info);
  fclose(f);
}

typedef struct { FrameList frames; uint32_t addr, len; } MemDump;

// Profile: cycles by instruction address, in a hash table.
enum { kProfSize = 1 << 18 };
static uint32_t *g_prof_addr, *g_prof_cycles;
static void ProfAdd(uint32_t pc, uint32_t cycles) {
  if (!g_prof_addr) {
    g_prof_addr = calloc(kProfSize, 4);
    g_prof_cycles = calloc(kProfSize, 4);
  }
  uint32_t h = (pc * 2654435761u) >> 14;
  for (;; h = (h + 1) & (kProfSize - 1)) {
    if (g_prof_addr[h] == pc || g_prof_addr[h] == 0) {
      g_prof_addr[h] = pc;
      g_prof_cycles[h] += cycles;
      return;
    }
  }
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: smrun CART [-n FRAMES] [-k SCRIPT] [-s SHOTS] [-o DIR] [-q] [-m FRAMES:ADDR:LEN] [-a AUDIO.raw]\n");
    return 1;
  }
  int nframes = 600;
  const char *script = "", *outdir = ".", *audio_path = NULL, *menu_script = NULL;
  FrameList shots = { 0 };
  MemDump dumps[16];
  int ndumps = 0;
  int prof_from = -1, prof_to = -1;
  uint32_t frame_addr = 0, ram_addr = 0;
  bool keep_save = false;
  struct { int frame; char *spec; bool done; } pokes[16];
  int npokes = 0;
  for (int i = 2; i < argc; i++) {
    const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : "";
    if (!strcmp(a, "-n")) nframes = atoi(v), i++;
    else if (!strcmp(a, "-k")) script = v, i++;
    else if (!strcmp(a, "-K")) menu_script = v, i++;
    else if (!strcmp(a, "-s")) ParseFramesInto(&shots, v), i++;
    else if (!strcmp(a, "-o")) outdir = v, i++;
    else if (!strcmp(a, "-a")) audio_path = v, i++;
    else if (!strcmp(a, "-q")) g_quiet = true;
    else if (!strcmp(a, "-S")) keep_save = true;
    else if (!strcmp(a, "-p")) { sscanf(v, "%d-%d", &prof_from, &prof_to); i++; }
    else if (!strcmp(a, "-f")) frame_addr = strtoul(v, NULL, 16), i++;
    else if (!strcmp(a, "-r")) ram_addr = strtoul(v, NULL, 16), i++;
    else if (!strcmp(a, "-w") && npokes < 16) {
      pokes[npokes].frame = atoi(v);
      pokes[npokes].spec = strchr(v, ':') ? strchr(v, ':') + 1 : "";
      pokes[npokes].done = false;
      npokes++, i++;
    }
    else if (!strcmp(a, "-m") && ndumps < 16) {
      char *spec = strdup(v), *c1 = strchr(spec, ':'), *c2 = c1 ? strchr(c1 + 1, ':') : NULL;
      if (!c2) { fprintf(stderr, "bad -m %s\n", v); return 1; }
      *c1 = *c2 = 0;
      memset(&dumps[ndumps], 0, sizeof(MemDump));
      ParseFramesInto(&dumps[ndumps].frames, spec);
      dumps[ndumps].addr = strtoul(c1 + 1, NULL, 16);
      dumps[ndumps].len = strtoul(c2 + 1, NULL, 0);
      ndumps++, i++;
    } else { fprintf(stderr, "bad arg %s\n", a); return 1; }
  }
  // The menu's script first, then the game's; the game's starts at
  // menu_steps.
  int menu_steps = 0, menu_frames = 0;
  if (menu_script) {
    ParseScript(menu_script, 0);
    menu_steps = g_nsteps;
    for (int i = 0; i < g_nsteps; i++) menu_frames += g_steps[i].frames;
  }
  ParseScript(script, 0);

  mLogSetDefaultLogger(&g_logger);
  struct mCore *core = mCoreFind(argv[1]);
  if (!core || !core->init(core)) { fprintf(stderr, "can't init core\n"); return 1; }
  mCoreInitConfig(core, NULL);
  struct mCoreOptions opts = { .useBios = false, .skipBios = true, .volume = 0x100,
                               .sampleRate = 32768, .audioBuffers = 1024 };
  mCoreConfigLoadDefaults(&core->config, &opts);
  mCoreLoadConfig(core);
  unsigned w, h;
  core->baseVideoSize(core, &w, &h);
  uint32_t *pixels = calloc(w * h, 4);
  core->setVideoBuffer(core, (mColor *)pixels, w);
  if (!mCoreLoadFile(core, argv[1])) { fprintf(stderr, "can't load %s\n", argv[1]); return 1; }
  if (keep_save) mCoreAutoloadSave(core);
  core->setAudioBufferSize(core, 2048);
  core->reset(core);

  FILE *af = audio_path ? fopen(audio_path, "wb") : NULL;
  int16_t abuf[4096 * 2];
  char path[512];
  int step = 0, left = g_nsteps ? g_steps[0].frames : 0;
  int mstep = 0, mleft = menu_steps ? g_steps[0].frames : 0;
  if (frame_addr && menu_steps) {
    step = menu_steps;
    left = step < g_nsteps ? g_steps[step].frames : 0;
  }
  uint32_t script_pos = 0;  // game frames the script has been advanced by
  int next_shot = 0;
  for (g_frame = 0; g_frame < nframes; g_frame++) {
    uint32_t keys = 0;
    if (frame_addr && core->busRead32(core, frame_addr) == 0 && mstep < menu_steps) {
      // Before the game: the menu's script, by GBA frame.
      keys = g_steps[mstep].keys;
      if (--mleft <= 0 && ++mstep < menu_steps) mleft = g_steps[mstep].frames;
    } else if (frame_addr) {
      // Keys for the next game frame to start.
      uint32_t gf = core->busRead32(core, frame_addr);
      while (script_pos < gf) {
        script_pos++;
        if (step < g_nsteps && --left <= 0 && ++step < g_nsteps) left = g_steps[step].frames;
      }
      if (step < g_nsteps) keys = g_steps[step].keys;
      for (int p = 0; p < npokes; p++) {
        if (pokes[p].done || (uint32_t)pokes[p].frame > gf) continue;
        pokes[p].done = true;
        for (char *q = pokes[p].spec; *q;) {
          unsigned addr = strtoul(q, &q, 16), val = 0;
          if (*q == '=') val = strtoul(q + 1, &q, 16);
          core->busWrite8(core, ram_addr + (addr & 0x1ffff), val);
          if (*q == ',') q++; else break;
        }
      }
    } else if (step < g_nsteps) {
      keys = g_steps[step].keys;
      if (--left <= 0 && ++step < g_nsteps) left = g_steps[step].frames;
    }
    core->setKeys(core, keys);
    if (g_frame >= prof_from && g_frame <= prof_to) {
      // Step one instruction at a time, charging the cycles to its address.
      struct ARMCore *cpu = core->cpu;
      uint32_t start = core->frameCounter(core);
      while (core->frameCounter(core) == start) {
        uint32_t pc = cpu->gprs[15] - (cpu->executionMode == MODE_THUMB ? 2 : 4);
        int32_t c0 = cpu->cycles;
        core->step(core);
        int32_t dc = cpu->cycles - c0;
        if (dc < 0 || dc > 1000) dc = 1;
        ProfAdd(pc, dc);
      }
    } else {
      core->runFrame(core);
    }
    if (af) {
      struct mAudioBuffer *ab = core->getAudioBuffer(core);
      size_t n;
      while ((n = mAudioBufferRead(ab, abuf, 4096)) > 0)
        fwrite(abuf, 4, n, af);
    }
    if (frame_addr) {
      // Game frame N shows once game frame N + 1 has started.
      uint32_t gf = core->busRead32(core, frame_addr);
      while (next_shot < shots.n && (uint32_t)shots.v[next_shot] + 2 <= gf) {
        if ((uint32_t)shots.v[next_shot] + 2 == gf) {
          snprintf(path, sizeof(path), "%s/g%06d.png", outdir, shots.v[next_shot]);
          WritePng(path, pixels, w, h);
        }
        next_shot++;
      }
    } else if (InList(&shots, g_frame)) {
      snprintf(path, sizeof(path), "%s/g%06d.png", outdir, g_frame);
      WritePng(path, pixels, w, h);
    }
    uint32_t gf_now = frame_addr ? core->busRead32(core, frame_addr) : 0;
    for (int d = 0; d < ndumps; d++) {
      // With -f, dump frames are game frames, taken when its shot would be.
      if (frame_addr ? !InList(&dumps[d].frames, (int)gf_now - 2) : !InList(&dumps[d].frames, g_frame))
        continue;
      snprintf(path, sizeof(path), "%s/m%06d_%08x.bin", outdir, g_frame, dumps[d].addr);
      FILE *f = fopen(path, "wb");
      for (uint32_t i = 0; i < dumps[d].len; i++) fputc(core->rawRead8(core, dumps[d].addr + i, -1), f);
      fclose(f);
    }
  }
  if (af) fclose(af);
  if (g_prof_addr) {
    snprintf(path, sizeof(path), "%s/profile.txt", outdir);
    FILE *f = fopen(path, "w");
    for (int i = 0; i < kProfSize; i++)
      if (g_prof_addr[i]) fprintf(f, "%08x %u\n", g_prof_addr[i], g_prof_cycles[i]);
    fclose(f);
  }
  core->deinit(core);
  return 0;
}
