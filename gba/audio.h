#pragma once
#include <stdint.h>
void AudioInit(void);
// Starts the sound driver for a game, with its sound RAM image.
void AudioStart(uint32_t spc_tag);
// Hands the ports the game wrote this frame to the driver.
void AudioEndFrame(void);
// Mixes the next frame of sound; called from the VBlank interrupt.
void AudioVBlank(void);
