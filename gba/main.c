// The GBA port of Super Mario All-Stars' Super Mario Bros. and The Lost
// Levels: startup, the frame loop and the view onto the SNES's picture.
#include <string.h>
#include <stdio.h>
#include "gba_hw.h"
#include "platform.h"
#include "snes.h"
#include "render.h"
#include "game.h"
#include "audio.h"
#include "irq.h"
#include "menu.h"

// SMB1's RAM (the same addresses in both games).
#define RAM8(a) (g_ram[a])
enum {
  kRam_PlayerYHi = 0xbb,
  kRam_PlayerYLo = 0x237,
  kRam_GameMode = 0x770,
  kRam_GameModeTask = 0x772,
  kRam_PlayerSize = 0x754,  // 0 big, 1 small
  kRam_PauseMenu = 0x776,   // display_pause_menu_flag (both games)
};

// The GBA's buttons as the SNES joypad word (B Y Sel Start U D L R A X L R).
// A jumps and B runs, as on the NES: they are the SNES's B and Y.
static uint16_t ReadJoypad(void) {
  uint16_t k = ~REG_KEYINPUT & 0x3ff;
  uint16_t j = 0;
  if (k & KEY_A) j |= 0x8000;
  if (k & KEY_B) j |= 0x4000;
  if (k & KEY_SELECT) j |= 0x2000;
  if (k & KEY_START) j |= 0x1000;
  if (k & KEY_UP) j |= 0x0800;
  if (k & KEY_DOWN) j |= 0x0400;
  if (k & KEY_LEFT) j |= 0x0200;
  if (k & KEY_RIGHT) j |= 0x0100;
  if (k & KEY_L) j |= 0x0020;
  if (k & KEY_R) j |= 0x0010;
  if ((j & 0x0c00) == 0x0c00) j &= ~0x0c00;
  if ((j & 0x0300) == 0x0300) j &= ~0x0300;
  return j;
}

// The view. In a level: the whole 160 lines are the level, with BG3's HUD
// rows (SNES rows 14-30: its two rows of text, from the line above their
// outline to the outline's last) drawn over its top, as the Super Mario
// Advance games do; the camera rests at the bottom, so the ground shows, and
// rises only when Mario climbs or jumps high. The title and the screens
// between levels: without the HUD, squashed a little (see render.h).
enum {
  kOverlayLines = 17, kOverlayTop = 14, // the overlaid HUD
  kMaxOffset = 64,                      // SNES rows 64-223: the floor at the bottom
  kHeadroom = 40,                       // lines kept above Mario's head
};
static int s_cam = kMaxOffset;

// The kind of view: the title's, a level's, or the screens' between levels.
// It changes only while the screen is dark (or, should it never be, after
// two thirds of a second): a level fading out after a death must not be
// squashed on the way.
enum { kViewTitle, kViewLevel, kViewBetween };
static uint8_t s_view_kind = kViewTitle, s_view_wait;

static void UpdateView(ViewMap *vm) {
  vm->xoffset = 8;
  vm->hud_overlay = false;
  vm->squash = kSquashNone;
  uint8_t mode = RAM8(kRam_GameMode), task = RAM8(kRam_GameModeTask);
  int want = mode == 0 ? kViewTitle
           : ((mode == 1 && task >= 3) || mode == 2) ? kViewLevel : kViewBetween;
  if (want != s_view_kind) {
    uint8_t inidisp = g_snes->ppu.inidisp;
    if ((inidisp & 0x80) || !(inidisp & 0x0f) || ++s_view_wait > 40)
      s_view_kind = want;
  }
  if (want == s_view_kind) s_view_wait = 0;
  if (s_view_kind == kViewTitle) {
    // The title: all but the HUD, squashed a little.
    vm->squash = kSquashTitle;
    return;
  }
  if (s_view_kind == kViewLevel) {
    vm->split = kOverlayLines;
    vm->top_offset = kOverlayTop;
    vm->hud_overlay = true;
    // Mario's box is 32 lines tall from his y; small Mario is its lower half.
    int y = (RAM8(kRam_PlayerYHi) - 1) * 256 + RAM8(kRam_PlayerYLo);
    int head = y + (RAM8(kRam_PlayerSize) ? 16 : 0);
    int target = kMaxOffset;
    if (head - target < kHeadroom) target = head - kHeadroom;
    // The pause menu's box (SNES rows 64-135) clear of the HUD.
    if (RAM8(kRam_PauseMenu) & 1) target = 40;
    // Below the overlaid HUD the level shows rows from offset + 16; above
    // row 32, BG3 is the SNES's own HUD, which would show twice.
    if (target < 32 - kOverlayLines) target = 32 - kOverlayLines;
    // Ease toward it: up quickly (Mario can move 5 lines a frame), down slowly.
    if (target < s_cam) s_cam -= (s_cam - target > 6) ? 6 : s_cam - target;
    else if (target > s_cam) s_cam += (target - s_cam > 3) ? 3 : target - s_cam;
    vm->offset = s_cam;
    return;
  }
  // The intro card and the other screens between levels: without the HUD
  // too, giving up their empty rows.
  vm->squash = kSquashFull;
  s_cam = kMaxOffset;
}

// Waits for the VBlank to present in. If one began since the last present and
// there's still time left in it, it's used: a frame slightly over budget
// borrows from the next instead of waiting a whole frame (see the porting
// guide, section 6).
static uint32_t s_last_present_vblank;
static void WaitForVBlank(void) {
  uint16_t line = REG_VCOUNT;
  if (g_vblank_count != s_last_present_vblank && line >= 160 && line < 200)
    return;
  VBlankIntrWait();
}

// A, B, Select and Start together: back to the game select, as a soft reset.
static bool SoftResetHeld(void) {
  uint16_t k = ~REG_KEYINPUT & (KEY_A | KEY_B | KEY_SELECT | KEY_START);
  return k == (KEY_A | KEY_B | KEY_SELECT | KEY_START);
}

static void RunGame(void) {
  ViewMap vm;
  uint32_t frames = 0, vb_start = g_vblank_count;
  uint32_t t_game = 0, t_build = 0, t_present = 0;
  for (;;) {
    if (SoftResetHeld()) {
      GameSyncSram(true);
      AudioStop();
      RenderStop();
      return;
    }
    uint32_t t0 = CycleCount();
    GameRunFrame(ReadJoypad());
    uint32_t t1 = CycleCount();
    UpdateView(&vm);
    RenderBuildFrame(&vm);
    uint32_t t2 = CycleCount();
    WaitForVBlank();
    s_last_present_vblank = g_vblank_count;
    uint32_t t3 = CycleCount();
    RenderPresent();
    g_shown_frame = g_game_frame;
    uint32_t t4 = CycleCount();
    GameSyncSram(false);
    t_game += t1 - t0, t_build += t2 - t1, t_present += t4 - t3;
    if (++frames == 60) {
      printf("60 frames in %u vblanks: game %u build %u present %u",
             (unsigned)(g_vblank_count - vb_start), (unsigned)(t_game / 60),
             (unsigned)(t_build / 60), (unsigned)(t_present / 60));
      frames = 0, vb_start = g_vblank_count;
      t_game = t_build = t_present = 0;
    }
  }
}

int main(void) {
  REG_WAITCNT = 0x4317;  // ROM 3/1 wait states with prefetch, SRAM 8
  REG_DISPCNT = DCNT_BLANK;
  // Timers 2 and 3: a cycle counter.
  REG_TM_CNT_H(2) = 0;
  REG_TM_CNT_H(3) = 0;
  REG_TM_CNT_L(2) = 0;
  REG_TM_CNT_L(3) = 0;
  REG_TM_CNT_H(3) = 0x84;
  REG_TM_CNT_H(2) = 0x80;

  IrqInit();
  RenderInit();
  AudioInit();
  printf("smas-gba: start");

  for (;;) {
    int choice = MenuRun();
    RenderInit();
    GameStart(&kGames[choice]);
    s_cam = kMaxOffset;
    s_view_kind = kViewTitle;
    RunGame();
  }
}
