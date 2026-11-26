// Sound: snesrev's decompiled SPC700 driver (smb1_spc_player.c, shared by
// both games) and the mixer standing in for the S-DSP.
//
// The driver runs at the end of each game frame, for 534 of the DSP's
// samples, ticking every 64 as the PC version's SmasSpcPlayer_GenerateSamples
// does, so what the game reads back from the APU ports is the same as on the
// PC, frame for frame. Its DSP register writes go into a queue, stamped with
// their time in the frame; the mixer, in an interrupt at the top of every
// GBA frame, plays one game frame's writes at those times while it mixes the
// next 224 samples. The sound DMA plays the other buffer and swaps at
// VBlank, so a slow game frame never starves the sound hardware.
#include <string.h>
#include <stdio.h>
#include "gba_hw.h"
#include "platform.h"
#include "mixer.h"
#include "audio.h"
#include "../game/src/smw_spc_player.h"
#include "../game/src/gba_dsp.h"

SpcPlayer *SmasSpcPlayer_CreateWithRam(uint8_t *ram);
void SmasSpcPlayer_Tick(SpcPlayer *p);

enum {
  kDspSamplesPerFrame = 534,
  kMaxCommands = 512,
  kQueueFrames = 4,
};

typedef struct DspCommand { uint16_t time; uint8_t reg, val; } DspCommand;
typedef struct CommandFrame { uint16_t count; DspCommand cmd[kMaxCommands]; } CommandFrame;

EWRAM_BSS static CommandFrame s_queue[kQueueFrames];
static volatile uint8_t s_q_head, s_q_count;   // frames waiting for the mixer
static CommandFrame *s_filling;
static uint16_t s_time;                         // DSP samples into the frame
static uint8_t s_timer_cycles;

static SpcPlayer *s_player;
static uint8_t s_pending_ports[4];
static bool s_started;

EWRAM_BSS static int8_t s_out[2][2][kMixSamplesPerFrame] __attribute__((aligned(4)));
static int s_playing;

// ---- the DSP interface the driver calls ----

struct Dsp { int unused; };
static uint8_t s_dsp_shadow[128];
static struct Dsp s_dsp_handle;

Dsp *dsp_init(uint8_t *ram) { return &s_dsp_handle; }

void dsp_reset(Dsp *dsp) {
  memset(s_dsp_shadow, 0, sizeof(s_dsp_shadow));
  s_dsp_shadow[0x6c] = 0xe0;
  // The driver resets the DSP while it initializes, before the mixer runs.
  if (s_filling && s_filling->count < kMaxCommands)
    s_filling->cmd[s_filling->count++] = (DspCommand){ s_time, 0x6c, 0xe0 };
}

void dsp_write(Dsp *dsp, uint8_t adr, uint8_t val) {
  if (!s_filling) return;
  adr &= 0x7f;
  // The driver writes most registers every tick, mostly unchanged. KON and
  // KOF act when written (with a bit set), so those always go through.
  if (adr == 0x4c || adr == 0x5c) {
    if (!val) return;
  } else {
    if (s_dsp_shadow[adr] == val) return;
    s_dsp_shadow[adr] = val;
  }
  if (s_filling->count < kMaxCommands)
    s_filling->cmd[s_filling->count++] = (DspCommand){ s_time, adr, val };
}

// ---- the game's side ----

void ApuWritePort(int port, uint8_t value) { s_pending_ports[port] = value; }

uint8_t ApuReadPort(int port) { return s_player ? s_player->port_to_snes[port] : 0; }

void RtlApuUpload(const uint8_t *p) {
  // The sound RAM image is made at cartridge-build time; the upload resets
  // the driver's state, as on the PC.
  if (s_player) s_player->upload(s_player, p);
}

static CommandFrame *BeginCommandFrame(void) {
  // If the mixer is behind, the newest waiting frame takes the new writes.
  int n = s_q_count;
  if (n >= kQueueFrames) {
    CommandFrame *f = &s_queue[(s_q_head + n - 1) % kQueueFrames];
    return f;
  }
  CommandFrame *f = &s_queue[(s_q_head + n) % kQueueFrames];
  f->count = 0;
  return f;
}

static void EndCommandFrame(CommandFrame *f) {
  if (s_q_count < kQueueFrames) {
    REG_IME = 0;
    s_q_count++;
    REG_IME = 1;
  }
}

void AudioStart(uint32_t spc_tag) {
  uint32_t pcm_tag = spc_tag == ASSET_TAG('S', 'P', 'C', '1') ? ASSET_TAG('P', 'C', 'M', '1')
                                                             : ASSET_TAG('P', 'C', 'M', 'L');
  uint8_t *ram = (uint8_t *)FindAsset(spc_tag, NULL);
  const uint8_t *pcm = FindAsset(pcm_tag, NULL);
  if (!ram || !pcm) {
    printf("audio: missing sound assets");
    return;
  }
  REG_IME = 0;
  s_q_head = s_q_count = 0;
  s_started = false;
  REG_IME = 1;
  MixerInit(pcm);
  s_player = SmasSpcPlayer_CreateWithRam(ram);
  memset(s_pending_ports, 0, sizeof(s_pending_ports));
  s_time = 0;
  s_timer_cycles = 0;
  s_filling = BeginCommandFrame();
  s_player->initialize(s_player);
  EndCommandFrame(s_filling);
  s_filling = NULL;
  s_started = true;
}

void AudioStop(void) {
  REG_IME = 0;
  s_started = false;
  s_q_head = s_q_count = 0;
  REG_IME = 1;
  s_player = NULL;
  memset(s_out, 0, sizeof(s_out));
}

void AudioEndFrame(void) {
  if (!s_player) return;
  memcpy(s_player->input_ports, s_pending_ports, 4);
  s_filling = BeginCommandFrame();
  // As SmasSpcPlayer_GenerateSamples: 534 DSP samples, a tick every 64.
  uint16_t done = 0;
  for (;;) {
    if (s_timer_cycles >= 64) {
      s_time = done;
      SmasSpcPlayer_Tick(s_player);
      s_timer_cycles &= 63;
    }
    int n = kDspSamplesPerFrame - done;
    if (n > 64 - s_timer_cycles) n = 64 - s_timer_cycles;
    s_timer_cycles += n;
    done += n;
    if (done == kDspSamplesPerFrame) break;
  }
  EndCommandFrame(s_filling);
  s_filling = NULL;
}

// ---- the interrupt side ----

void AudioInit(void) {
  REG_SOUNDCNT_X = 0x80;
  // DMA sound A to the left, B to the right, both at full volume, timer 0.
  REG_SOUNDCNT_H = 0x0004 | 0x0008 | 0x0200 | 0x0800 | 0x1000 | 0x8000;
  memset(s_out, 0, sizeof(s_out));
  REG_TM_CNT_H(0) = 0;
  REG_TM_CNT_L(0) = kMixTimerReload;
  REG_TM_CNT_H(0) = 0x80;
  s_playing = 0;
  AudioVBlank();
}

// At VBlank: the sound DMA starts on the buffer mixed during this frame.
void AudioVBlank(void) {
  int b = s_playing;
  REG_DMA_CNT(1) = 0;
  REG_DMA_CNT(2) = 0;
  REG_DMA_SAD(1) = (uint32_t)s_out[b][0];
  REG_DMA_DAD(1) = REG_FIFO_A;
  REG_DMA_SAD(2) = (uint32_t)s_out[b][1];
  REG_DMA_DAD(2) = REG_FIFO_B;
  REG_DMA_CNT(1) = DMA_DST_FIXED | DMA_REPEAT | DMA_32 | DMA_AT_FIFO | DMA_ENABLE;
  REG_DMA_CNT(2) = DMA_DST_FIXED | DMA_REPEAT | DMA_32 | DMA_AT_FIFO | DMA_ENABLE;
  s_playing ^= 1;
}

// At the top of the frame: mix the next frame into the buffer not playing.
void AudioMix(void) {
  if (!s_started) return;
  int out = 0;
  if (s_q_count) {
    CommandFrame *f = &s_queue[s_q_head];
    for (int i = 0; i < f->count; i++) {
      const DspCommand *c = &f->cmd[i];
      // time * 224 / 534, as a multiply: 224/534 is 27491/65536 to 1 part in 10^5.
      int o = (c->time * 27491) >> 16;
      if (o > out) {
        MixerRun(out, o);
        out = o;
      }
      MixerWrite(c->reg, c->val);
    }
    s_q_head = (s_q_head + 1) % kQueueFrames;
    s_q_count--;
  }
  MixerRun(out, kMixSamplesPerFrame);
  int b = s_playing;  // the one VBlank will start next
  MixerOutput(s_out[b][0], s_out[b][1]);
}
