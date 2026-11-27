// Running the decompiled games: a frame is the game's main loop, its NMI
// handler, then the frame's lines with HDMA and the IRQ handler (snes_hw.c).
#include <string.h>
#include <stdio.h>
#include "gba_hw.h"
#include "platform.h"
#include "snes.h"
#include "game.h"
#include "audio.h"

void Smb1VectorReset(void);
void Smb1RunOneFrameOfGame_Internal(void);
void Smb1VectorNMI(void);
void Smb1VectorIRQ(void);
void Smbll_VectorReset(void);
void SmbllRunOneFrameOfGame_Internal(void);
void Smbll_VectorNMI(void);
void Smbll_VectorIRQ(void);
bool g_smb1_want_reset, g_smbll_want_reset;

const GameDef kGames[2] = {
  { "SUPER MARIO BROS.", ASSET_TAG('S', 'M', 'B', '1'), ASSET_TAG('S', 'P', 'C', '1'), 0x0000,
    Smb1VectorReset, Smb1RunOneFrameOfGame_Internal, Smb1VectorNMI, Smb1VectorIRQ,
    &g_smb1_want_reset },
  { "THE LOST LEVELS", ASSET_TAG('S', 'M', 'B', 'L'), ASSET_TAG('S', 'P', 'C', 'L'), 0x2000,
    Smbll_VectorReset, SmbllRunOneFrameOfGame_Internal, Smbll_VectorNMI, Smbll_VectorIRQ,
    &g_smbll_want_reset },
};
const GameDef *g_game;
// Game frames run so far; tools/mgba/smrun's -f follows it.
volatile uint32_t g_game_frame;
// The game frame whose picture was last presented; smrun's -F follows it.
volatile uint32_t g_shown_frame;

EWRAM_BSS static uint8_t s_sram[kSramSize];
static bool s_need_reset;
static int s_sync_pos;

void GameStart(const GameDef *game) {
  g_game = game;
  g_rom = FindAsset(game->rom_tag, &g_rom_size);
  if (!g_rom) {
    printf("missing asset for %s", game->title);
    for (;;) {}
  }
  g_sram = s_sram;
  for (int i = 0; i < kSramSize; i++) s_sram[i] = SRAM[game->sram_offset + i];
  memset(g_ram, 0, sizeof(g_ram));
  SnesReset();
  AudioStart(game->spc_tag);
  s_need_reset = true;
}

bool GameRunFrame(uint16_t joy1) {
  g_game_frame++;
  g_snes->joy1 = joy1;
  if (s_need_reset) {
    s_need_reset = false;
    g_game->reset();
  }
  g_game->frame();
  if (*g_game->want_reset) {
    *g_game->want_reset = false;
    return true;
  }
  g_game->nmi();
  SnesRunLines(g_game->irq);
  AudioEndFrame();
  return false;
}

void GameSyncSram(bool all) {
  int n = all ? kSramSize : 16;
  uint32_t base = g_game->sram_offset;
  for (int i = 0; i < n; i++) {
    int a = s_sync_pos;
    s_sync_pos = (s_sync_pos + 1) & (kSramSize - 1);
    if (SRAM[base + a] != s_sram[a])
      SRAM[base + a] = s_sram[a];
  }
}

void RtlWriteSram(void) {
  GameSyncSram(true);
}
