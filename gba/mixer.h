// The GBA's stand-in for the SNES's S-DSP: eight voices playing the
// cartridge's decoded samples (tools/spc_image.py) with the DSP's envelopes,
// volumes, pitch and noise, mixed to 8-bit stereo.
#pragma once
#include <stdint.h>

enum {
  kMixRate = 13379,        // 16777216 / 1254: 224 samples a frame
  kMixSamplesPerFrame = 224,
  kMixTimerReload = 65536 - 1254,
};

void MixerInit(const uint8_t *pcm_asset);
// A DSP register write, as the S-DSP takes it.
void MixerWrite(uint8_t adr, uint8_t val);
// Mixes samples [from, to) of the frame's buffer.
void MixerRun(int from, int to);
// Turns the frame's mix into 8-bit samples for the sound DMA.
void MixerOutput(int8_t *left, int8_t *right);
