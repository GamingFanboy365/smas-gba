// The game select: Super Mario Bros. or The Lost Levels, drawn with SMB1's
// own HUD font from the player's ROM (its BG3 tiles at $09B000), with the
// cursor sound of All-Stars' own game select: sound 1 on the driver's port
// 3, which is the same driver, samples and sound data as SMB1's (compared
// byte for byte with the SPC700's RAM while the real menu runs).
#include <string.h>
#include "gba_hw.h"
#include "platform.h"
#include "irq.h"
#include "audio.h"
#include "menu.h"

enum {
  kFontAddr = 0x09b000,      // SMB1's 2bpp HUD font, 128 tiles
  kCharBlock = 2,
  kScreenBlock = 28,
  kBlank = 0x28, kDash = 0x24, kCoin = 0x27,
  kSramChoice = 0x4000,      // after the two games' 8 KB each
  kCursorSound = 1,          // port 3
};

static const uint16_t kPalette[4] = {
  0,
  0x7fff,                    // the letters
  0x03bf,                    // gold (the coin)
  0x2861,                    // the outline
};

static uint8_t FontTile(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'Z') return 10 + c - 'A';
  if (c == '-') return kDash;
  if (c == '*') return kCoin;
  return kBlank;
}

static void PutText(int x, int y, const char *s, int pal) {
  volatile uint16_t *map = VRAM + kScreenBlock * 0x400;
  for (; *s; s++, x++) map[y * 32 + x] = FontTile(*s) | pal << 12;
}

static void LoadFont(const uint8_t *rom) {
  const uint8_t *src = rom + (((kFontAddr >> 16) << 15) | (kFontAddr & 0x7fff));
  volatile uint32_t *dst = (volatile uint32_t *)(VRAM + kCharBlock * 0x2000);
  for (int t = 0; t < 128; t++) {
    for (int y = 0; y < 8; y++) {
      uint8_t lo = src[t * 16 + y * 2], hi = src[t * 16 + y * 2 + 1];
      uint32_t w = 0;
      for (int x = 0; x < 8; x++) {
        uint32_t c = ((lo >> (7 - x)) & 1) | (((hi >> (7 - x)) & 1) << 1);
        w |= c << (4 * x);
      }
      dst[t * 8 + y] = w;
    }
  }
}

static void Fade(bool in) {
  REG_BLDCNT = 0x3f | 3 << 6;
  for (int i = 0; i <= 16; i += 2) {
    REG_BLDY = in ? 16 - i : i;
    VBlankIntrWait();
  }
}

int MenuRun(void) {
  uint32_t size;
  const uint8_t *rom = FindAsset(ASSET_TAG('S', 'M', 'B', '1'), &size);
  int choice = SRAM[kSramChoice] == 0xa1 ? 1 : 0;

  REG_DISPCNT = DCNT_BLANK;
  for (int i = 0; i < 4; i++) REG_BGCNT(i) = 0;
  for (int i = 0; i < 8; i++) ((volatile uint16_t *)0x04000010)[i] = 0;
  REG_DMA_CNT(0) = 0;
  REG_DMA_CNT(3) = 0;
  volatile uint16_t *map = VRAM + kScreenBlock * 0x400;
  for (int i = 0; i < 0x400; i++) map[i] = kBlank;
  if (rom) LoadFont(rom);
  for (int i = 0; i < 4; i++) {
    PAL_BG[i] = kPalette[i];
    PAL_BG[16 + i] = kPalette[i];
  }
  PAL_BG[16 + 1] = 0x4e73;   // palette 1: dimmed letters
  PAL_BG[0] = 0x6d84;        // the sky

  PutText(4, 4, "SUPER MARIO ALL-STARS", 0);
  PutText(11, 17, "A-START", 1);

  REG_BGCNT(0) = kCharBlock << 2 | kScreenBlock << 8;
  REG_DISPCNT = DCNT_MODE0 | DCNT_BG0;
  AudioStart(ASSET_TAG('S', 'P', 'C', '1'));
  bool drawn = false;
  uint8_t sound = 0;
  uint16_t prev = 0x3ff;
  for (int frame = 0;; frame++) {
    // The choices, the chosen one bright, with the coin beside it.
    if (!drawn) {
      PutText(4, 9, choice == 0 ? "*" : " ", 0);
      PutText(6, 9, "SUPER MARIO BROS", choice == 0 ? 0 : 1);
      PutText(4, 12, choice == 1 ? "*" : " ", 0);
      PutText(6, 12, "THE LOST LEVELS", choice == 1 ? 0 : 1);
      drawn = true;
    }
    if (frame == 0) Fade(true);
    VBlankIntrWait();
    // The sound driver's frame: a sound's number for a frame, then 0.
    ApuWritePort(3, sound);
    sound = 0;
    AudioEndFrame();
    uint16_t keys = ~REG_KEYINPUT & 0x3ff, pressed = keys & ~prev;
    prev = keys;
    if (pressed & (KEY_UP | KEY_DOWN | KEY_SELECT)) {
      choice ^= 1;
      drawn = false;
      sound = kCursorSound;
    }
    if (pressed & (KEY_A | KEY_START))
      break;
  }
  SRAM[kSramChoice] = choice ? 0xa1 : 0xa0;
  Fade(false);
  AudioStop();
  REG_DISPCNT = DCNT_BLANK;
  return choice;
}
