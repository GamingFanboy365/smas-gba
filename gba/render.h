#pragma once
#include <stdint.h>
#include <stdbool.h>

// How the SNES's 256x224 picture is fitted onto the GBA's 240x160.
// GBA line g shows SNES row g + top_offset for g < split (the HUD, pinned),
// and SNES row g + offset below it; columns are shifted left by xoffset.
typedef struct ViewMap {
  int xoffset;
  int split;        // GBA line where the second region starts (0 for none)
  int top_offset;
  int offset;
} ViewMap;

void RenderInit(void);
// Turns the SNES state the frame left into the GBA's next picture, in
// buffers; RenderPresent puts it on screen (at VBlank).
void RenderBuildFrame(const ViewMap *vm);
void RenderPresent(void);
