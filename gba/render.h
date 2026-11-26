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
  // Nonzero: the pause menu's sprites (the box and its letters) are shown
  // at this offset instead, in the middle of the screen, the level staying
  // where it is.
  int menu_offset;
  // Squashed instead (split and offsets unused): rows 32-223, without the
  // HUD, on the 160 lines; text rows are kept whole.
  //   kSquashTitle: the 32 rows to lose spread over all that isn't text;
  //   kSquashFull: blank rows go first, the margins before the gaps, so
  //     that a card's text and picture aren't squashed at all (the screens
  //     between levels).
  uint8_t squash;
} ViewMap;

enum { kSquashNone, kSquashFull, kSquashTitle };

void RenderInit(void);
// Turns the SNES state the frame left into the GBA's next picture, in
// buffers; RenderPresent puts it on screen (at VBlank).
void RenderBuildFrame(const ViewMap *vm);
void RenderPresent(void);
// Restarts the picture's per-line registers; called from the VBlank interrupt.
void RenderVBlank(void);
// The mid-picture change of the HUD layer's priority; from the VCount interrupt.
void RenderMidFrame(void);
// Stops them (leaving a game).
void RenderStop(void);
