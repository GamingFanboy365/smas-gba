// What the GBA platform layer shares between its files.
#pragma once
#include <stdint.h>
#include <stdbool.h>

enum { kSramSize = 0x2000 };

extern uint8_t g_ram[0x20000];
extern const uint8_t *g_rom;   // the running game's ROM image (LoROM)
extern uint32_t g_rom_size;
extern uint8_t *g_sram;

// The SNES APU ports, between the game and the sound driver (audio.c).
void ApuWritePort(int port, uint8_t value);
uint8_t ApuReadPort(int port);

// Register access as the game does it (snes_hw.c).
void WriteReg(uint16_t reg, uint8_t value);
void WriteRegWord(uint16_t reg, uint16_t value);
uint8_t ReadReg(uint16_t reg);
uint16_t ReadRegWord(uint16_t reg);

// The cartridge's assets (made by tools/make_cartridge.py), found by tag.
const void *FindAsset(uint32_t tag, uint32_t *size);
#define ASSET_TAG(a, b, c, d) ((a) | (b) << 8 | (c) << 16 | (uint32_t)(d) << 24)
