// The games in the cartridge and how a frame of one runs.
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct GameDef {
  const char *title;
  uint32_t rom_tag;        // the asset holding the game's ROM image
  uint32_t spc_tag;        // its sound RAM image
  uint32_t sram_offset;    // where its save data lives in the GBA's SRAM
  void (*reset)(void);
  void (*frame)(void);
  void (*nmi)(void);
  void (*irq)(void);
  bool *want_reset;
} GameDef;

extern const GameDef kGames[2];
extern const GameDef *g_game;

void GameStart(const GameDef *game);
void GameRunFrame(uint16_t joy1);
// Writes save data that changed to the GBA's SRAM, a slice a call.
void GameSyncSram(bool all);
extern volatile uint32_t g_game_frame, g_shown_frame;
