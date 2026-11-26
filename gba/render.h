#pragma once
#include <stdint.h>
#include <stdbool.h>

// How the SNES's 256x224 picture is fitted onto the GBA's 240x160.
// Columns are shifted left by xoffset; GBA line g shows SNES row g + offset.
// For g < split, it shows row g + top_offset instead: for every layer
// (a pinned strip), or with hud_overlay for BG3 alone, the HUD drawn over the
// level the way the Super Mario Advance games draw theirs.
typedef struct ViewMap {
  int xoffset;
  int split;
  int top_offset;
  int offset;
  bool hud_overlay;
  // Squashed instead (split and offsets unused):
  //   kSquashFull: the whole 224 rows in 160 lines, each 8-row tile row of
  //     the text layer losing its first and last row (screens between levels);
  //   kSquashTitle: rows 32-223, without the HUD, each tile row losing only
  //     its last row (the title).
  uint8_t squash;
} ViewMap;

enum { kSquashNone, kSquashFull, kSquashTitle };

void RenderInit(void);
// Turns the SNES state the frame left into the GBA's next picture, in
// buffers; RenderPresent puts it on screen (at VBlank).
void RenderBuildFrame(const ViewMap *vm);
void RenderPresent(void);
