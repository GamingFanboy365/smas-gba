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
  kRam_EnemyFlag = 0x10,    // enemy_flag[6]: the slot is in use
  kRam_EnemyId = 0x1c,      // enemy_id[6]
  kRam_EnemySprOffset = 0xb46,  // enemy_obj_id[6]: its OAM entries from 64 + this / 4
  kEnemyLakitu = 17,
  kRam_World = 0x75f,
  kRam_CutsceneFlag = 0x1680, // cutscene_var1680: Peach in every castle
};

// Whether the game shows a menu that Start confirms: the title and file
// select, the pause menu, the Game Over menu, and the castle's and the
// ending's text (PUSH START).
static bool InMenu(void) {
  uint8_t mode = RAM8(kRam_GameMode);
  return mode == 0 || mode == 2 || mode == 3 || (RAM8(kRam_PauseMenu) & 1);
}

// The GBA's buttons as the SNES joypad word (B Y Sel Start U D L R A X L R).
// A jumps and B runs, as on the NES: they are the SNES's B and Y; R runs
// too, as in Super Mario Advance 4. In a menu, A pressed there confirms, as
// Start does. What A is goes with the press: held from play into a menu
// (dying with it down, say) it stays a jump and picks nothing, and a press
// that confirmed a menu doesn't jump when the game resumes under it.
static bool s_a_down, s_a_is_start;
static uint16_t ReadJoypad(void) {
  uint16_t k = ~REG_KEYINPUT & 0x3ff;
  uint16_t j = 0;
  if (k & KEY_A) {
    if (!s_a_down) s_a_is_start = InMenu();
    j |= s_a_is_start ? 0x1000 : 0x8000;
  }
  s_a_down = (k & KEY_A) != 0;
  if (k & (KEY_B | KEY_R)) j |= 0x4000;
  if (k & KEY_SELECT) j |= 0x2000;
  if (k & KEY_START) j |= 0x1000;
  if (k & KEY_UP) j |= 0x0800;
  if (k & KEY_DOWN) j |= 0x0400;
  if (k & KEY_LEFT) j |= 0x0200;
  if (k & KEY_RIGHT) j |= 0x0100;
  if (k & KEY_L) j |= 0x0020;
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
  kVictoryOffset = 55,                  // SNES rows 55-214 after the axe
  kEndingOffset = 47,                   // SNES rows 47-206: the ending's text in the middle
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
  vm->split = 0;
  vm->hud_overlay = false;
  vm->squash = kSquashNone;
  vm->menu_offset = 0;
  vm->pin_count = 0;
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
    // The ending's text, Peach's (her cutscene's task 8 on, in both games):
    // SNES rows 64-189 with PUSH START, too tall for the HUD and it both,
    // so it's shown without the HUD, in the middle of the screen.
    bool ending_text = mode == 2 && task >= 8 &&
                       (RAM8(kRam_World) >= 7 || RAM8(kRam_CutsceneFlag));
    if (!ending_text) {
      vm->split = kOverlayLines;
      vm->top_offset = kOverlayTop;
      vm->hud_overlay = true;
    }
    // Mario's box is 32 lines tall from his y; small Mario is its lower half.
    int y = (RAM8(kRam_PlayerYHi) - 1) * 256 + RAM8(kRam_PlayerYLo);
    int head = y + (RAM8(kRam_PlayerSize) ? 16 : 0);
    int target = kMaxOffset;
    if (head - target < kHeadroom) target = head - kHeadroom;
    // Below the overlaid HUD the level shows rows from offset + 16; above
    // row 32, BG3 is the SNES's own HUD, which would show twice.
    if (target < 32 - kOverlayLines) target = 32 - kOverlayLines;
    // After the axe (victory mode): Toad's message (SNES rows 80-135) well
    // below the HUD, the floor's top (rows 208-214) still in view; the
    // ending's text, without the HUD, in the middle.
    if (mode == 2) target = ending_text ? kEndingOffset : kVictoryOffset;
    // The pause menu's box (SNES rows 64-143) is shown in the middle of the
    // screen (lines 40-119); the level stays put. (Over the ending's text,
    // after PUSH START, it stays where the SNES has it, on the text.)
    if ((RAM8(kRam_PauseMenu) & 1) && mode != 2) vm->menu_offset = 24;
    // Lakitu flies above the rows the view shows: his sprites (six) are
    // drawn just under the HUD instead, so he and his Spinies can be seen.
    for (int k = 0; k < 6; k++) {
      if (RAM8(kRam_EnemyFlag + k) && RAM8(kRam_EnemyId + k) == kEnemyLakitu) {
        int first = 64 + RAM8(kRam_EnemySprOffset + k) / 4;
        if (first + 6 <= 128) vm->pin_first = first, vm->pin_count = 6;
        break;
      }
    }
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
    if (GameRunFrame(ReadJoypad())) {
      // Save & Quit: back to the game select, as All-Stars goes back to its
      // own menu.
      GameSyncSram(true);
      AudioStop();
      RenderStop();
      return;
    }
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

// A flash cartridge's menu (or anything else that ran before) may have left
// the video memory and the I/O registers in any state. This puts what the
// program doesn't set up itself back to what a console starts a game with:
// interrupts, DMA, timers and sound off, the video registers zero, the
// palettes and VRAM clear, the sprites hidden. (Done here rather than with
// the BIOS's RegisterRamReset, which also resets the serial port's mode.)
static void HardwareReset(void) {
  REG_IME = 0;
  REG_DISPCNT = DCNT_BLANK;
  for (int i = 0; i < 4; i++) {
    REG_DMA_CNT(i) = 0;
    REG_TM_CNT_H(i) = 0;
  }
  REG_IE = 0;
  REG_IF = 0xffff;
  REG_SOUNDCNT_X = 0;
  // The BGs' controls and scrolling, the windows, mosaic and blending.
  for (uint32_t a = 0x04000008; a < 0x04000056; a += 2) REG16(a) = 0;
  volatile uint32_t *p = (volatile uint32_t *)PAL_BG;
  for (int i = 0; i < 0x400 / 4; i++) p[i] = 0;
  p = (volatile uint32_t *)VRAM;
  for (int i = 0; i < 0x18000 / 4; i++) p[i] = 0;
  p = (volatile uint32_t *)OAM;
  for (int i = 0; i < 0x400 / 4; i += 2) p[i] = 0x200, p[i + 1] = 0;
}

int main(void) {
  HardwareReset();
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
