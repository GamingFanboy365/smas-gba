// The mixer: the S-DSP's job on the GBA (see mixer.h).
//
// The driver writes DSP registers between its ticks (dsp_write); the mixer
// runs the voices between those writes, in chunks of a few samples, at
// kMixRate. Each voice plays a sample decoded at cartridge-build time, at
// its pitch, with the DSP's envelope (ADSR or GAIN, stepped at the DSP's
// 32 kHz rates, state by state) and its volumes, all as the S-DSP computes
// them (reference: snesrev's LakeSnes dsp.c), except:
//   - samples are stepped through at the mixer's rate with linear
//     interpolation instead of the DSP's Gaussian one;
//   - no echo, no pitch modulation;
//   - the sample the driver rewrites with random data at run time
//     (SomeRandomStuff, directory entry 0x20) is made here, from the
//     DSP's noise generator, filtered as its BRR blocks are.
#include <string.h>
#include "gba_hw.h"
#include "mixer.h"

enum {
  kNumSamples = 64,
  kChunk = 32,                      // output samples between envelope steps
  // DSP samples (32 kHz) per output sample, in 16.16.
  kDspPerOut = (int)(32000.0 * 65536 / kMixRate),
  // Pitch to a 16.16 step per output sample: pitch / 4096 * 32000 / rate.
  kPitchToStep = (int)(32000.0 * 16 * 256 / kMixRate),  // then >> 8
  kNoiseSrcn = 0x20,
};

static const uint16_t kRates[32] = {
  0, 2048, 1536, 1280, 1024, 768, 640, 512, 384, 320, 256, 192, 160, 128, 96, 80,
  64, 48, 40, 32, 24, 20, 16, 12, 10, 8, 6, 5, 4, 3, 2, 1,
};

typedef struct SampleEntry { uint32_t offset, length, loop; } SampleEntry;

enum { kAttack, kDecay, kSustain, kGain, kRelease };

typedef struct Voice {
  const int16_t *pcm;
  uint32_t pos, step, end, loop_back;  // 16.16; loop_back 0: the sample ends
  bool on, noise_src, lp_noise;
  uint8_t srcn, adsr1, adsr2, gain_reg, state;
  int8_t vol_l, vol_r;
  uint16_t pitch, counter;
  int32_t env, lp_old;
} Voice;

typedef struct MixerState {
  Voice v[8];
  int8_t mvol_l, mvol_r;
  uint8_t flg, non;
  uint16_t noise_counter;
  int32_t noise;
  uint32_t dsp_frac;                 // 16.16 DSP samples not yet stepped
} MixerState;

static MixerState s_dsp;
static const uint8_t *s_pcm_asset;
// The frame's sums, interleaved: L, R, L, R...
static int32_t s_mix[kMixSamplesPerFrame * 2];

typedef struct MixJob {
  const int16_t *pcm;
  uint32_t pos, step, end, loop_back;
  int32_t gl, gr;
} MixJob;
int MixNearest(MixJob *job, int32_t *mix, int count);  // mixer_asm.S
int MixLinear(MixJob *job, int32_t *mix, int count);

// ---- the DSP's registers ----

ROM_CODE static void MixerReset(void) {
  MixerState *d = &s_dsp;
  memset(d, 0, sizeof(*d));
  d->flg = 0xe0;
  d->noise = -0x4000;
}

ROM_CODE static void KeyOn(Voice *v) {
  const SampleEntry *e = (const SampleEntry *)(s_pcm_asset + 4) + (v->srcn < kNumSamples ? v->srcn : 0);
  v->lp_noise = v->srcn == kNoiseSrcn;
  if (v->srcn >= kNumSamples || (!e->length && !v->lp_noise)) {
    v->on = false;
    return;
  }
  v->pcm = (const int16_t *)(s_pcm_asset + e->offset);
  v->pos = 0;
  v->end = e->length << 16;
  v->loop_back = e->loop == 0xffffffff ? 0 : (e->length - e->loop) << 16;
  v->lp_old = 0;
  v->env = 0;
  v->counter = 0;
  v->state = (v->adsr1 & 0x80) ? kAttack : kGain;
  v->on = true;
}

ROM_CODE void MixerWrite(uint8_t adr, uint8_t val) {
  MixerState *d = &s_dsp;
  Voice *v = &d->v[adr >> 4];
  switch (adr & 0xf) {
  case 0: v->vol_l = val; return;
  case 1: v->vol_r = val; return;
  case 2: v->pitch = (v->pitch & 0x3f00) | val; v->step = (v->pitch * kPitchToStep) >> 8; return;
  case 3: v->pitch = ((v->pitch & 0xff) | val << 8) & 0x3fff; v->step = (v->pitch * kPitchToStep) >> 8; return;
  case 4: v->srcn = val; return;
  case 5: v->adsr1 = val; return;
  case 6: v->adsr2 = val; return;
  case 7: v->gain_reg = val; return;
  case 0xc:
    switch (adr) {
    case 0x0c: d->mvol_l = val; return;
    case 0x1c: d->mvol_r = val; return;
    case 0x4c:
      for (int i = 0; i < 8; i++)
        if (val & (1 << i)) KeyOn(&d->v[i]);
      return;
    case 0x5c:
      for (int i = 0; i < 8; i++)
        if (val & (1 << i)) d->v[i].state = kRelease;
      return;
    case 0x6c:
      d->flg = val;
      if (val & 0x80)
        for (int i = 0; i < 8; i++) d->v[i].state = kRelease, d->v[i].env = 0;
      return;
    }
    return;
  case 0xd:
    if (adr == 0x3d) {
      d->non = val;
      for (int i = 0; i < 8; i++) d->v[i].noise_src = (val >> i) & 1;
    }
    return;
  }
}

// ---- envelopes ----

static void EnvStep(Voice *v) {
  switch (v->state) {
  case kAttack:
    v->env += (v->adsr1 & 0xf) == 0xf ? 1024 : 32;
    if (v->env >= 0x7e0) v->state = kDecay;
    if (v->env > 0x7ff) v->env = 0x7ff;
    break;
  case kDecay:
    v->env -= ((v->env - 1) >> 8) + 1;
    if (v->env < (((v->adsr2 >> 5) + 1) << 8)) v->state = kSustain;
    break;
  case kSustain:
    v->env -= ((v->env - 1) >> 8) + 1;
    break;
  case kGain:
    switch ((v->gain_reg >> 5) & 3) {
    case 0: v->env -= 32; if (v->env < 0) v->env = 0; break;
    case 1: v->env -= ((v->env - 1) >> 8) + 1; break;
    case 2: v->env += 32; if (v->env > 0x7ff) v->env = 0x7ff; break;
    case 3: v->env += v->env < 0x600 ? 32 : 8; if (v->env > 0x7ff) v->env = 0x7ff; break;
    }
    break;
  }
}

// Advances a voice's envelope by n of the DSP's samples.
static void EnvAdvance(Voice *v, int n) {
  if (v->state == kRelease) {
    v->env -= 8 * n;
    if (v->env <= 0) v->env = 0, v->on = false;
    return;
  }
  if (v->state == kGain && !(v->gain_reg & 0x80)) {
    v->env = (v->gain_reg & 0x7f) << 4;  // direct gain
    return;
  }
  for (;;) {
    int rate;
    switch (v->state) {
    case kAttack: rate = kRates[(v->adsr1 & 0xf) * 2 + 1]; break;
    case kDecay: rate = kRates[((v->adsr1 >> 4) & 7) * 2 + 16]; break;
    case kSustain: rate = kRates[v->adsr2 & 0x1f]; break;
    default: rate = kRates[v->gain_reg & 0x1f]; break;
    }
    if (!rate) return;
    int left = rate - v->counter;
    if (n < left) {
      v->counter += n;
      return;
    }
    n -= left;
    v->counter = 0;
    EnvStep(v);
  }
}

// ---- mixing ----

static void MixVoice(Voice *v, int32_t *mix, int count, const int16_t *noise) {
  int32_t gl = (v->env * v->vol_l) >> 7, gr = (v->env * v->vol_r) >> 7;
  if (v->noise_src) {
    for (int i = 0; i < count; i++) {
      int32_t s = noise[i];
      mix[i * 2] += s * gl, mix[i * 2 + 1] += s * gr;
    }
    return;
  }
  uint32_t pos = v->pos, step = v->step;
  if (v->lp_noise) {
    // Random BRR blocks, shift 9, filter 1: 4-bit noise through a one-pole
    // filter, a new value per sample at the voice's pitch.
    int32_t old = v->lp_old;
    for (int i = 0; i < count; i++) {
      mix[i * 2] += old * gl, mix[i * 2 + 1] += old * gr;
      uint32_t np = pos + step;
      for (uint32_t k = (np >> 16) - (pos >> 16); k; k--) {
        int32_t r = ((noise[i] >> 10) | 1) << 8;
        int32_t s = r + old + (-old >> 4);
        if (s > 0x7fff) s = 0x7fff; else if (s < -0x8000) s = -0x8000;
        old = (int16_t)((s & 0x7fff) << 1) >> 1;
      }
      pos = np & 0xffff;
    }
    v->lp_old = old;
    v->pos = pos;
    return;
  }
  MixJob job = { v->pcm, pos, step, v->end, v->loop_back, gl, gr };
  // Stepping over samples, interpolating buys nothing; well below the
  // mixer's rate (under half), it keeps the steps from buzzing.
  int alive = step >= 0x8000 ? MixNearest(&job, mix, count) : MixLinear(&job, mix, count);
  v->pos = job.pos;
  if (!alive) {
    v->on = false;
    v->env = 0;
  }
}

// The DSP's noise generator, one value per output sample.
ROM_CODE static void MakeNoise(int16_t *out, int count, int dsp_per_sample) {
  MixerState *d = &s_dsp;
  int rate = kRates[d->flg & 0x1f];
  int32_t n = d->noise;
  for (int i = 0; i < count; i++) {
    if (rate) {
      d->noise_counter += dsp_per_sample;
      while (d->noise_counter >= rate) {
        d->noise_counter -= rate;
        int bit = (n & 1) ^ ((n >> 1) & 1);
        n = ((n >> 1) & 0x3fff) | (bit << 14);
        n = (int16_t)((n & 0x7fff) << 1) >> 1;
      }
    }
    out[i] = n;
  }
  d->noise = n;
}

void MixerInit(const uint8_t *pcm_asset) {
  s_pcm_asset = pcm_asset;
  MixerReset();
}

void MixerRun(int from, int to) {
  MixerState *d = &s_dsp;
  static int16_t noise[kChunk];
  for (int at = from; at < to; at += kChunk) {
    int count = to - at < kChunk ? to - at : kChunk;
    d->dsp_frac += count * kDspPerOut;
    int ticks = d->dsp_frac >> 16;
    d->dsp_frac &= 0xffff;
    int32_t *mix = &s_mix[at * 2];
    for (int i = 0; i < count * 2; i++) mix[i] = 0;
    bool need_noise = d->non != 0;
    for (int k = 0; k < 8 && !need_noise; k++) need_noise = d->v[k].on && d->v[k].lp_noise;
    if (need_noise) MakeNoise(noise, count, (kDspPerOut + 0x8000) >> 16);
    for (int k = 0; k < 8; k++) {
      Voice *v = &d->v[k];
      if (!v->on) continue;
      EnvAdvance(v, ticks);
      if (!v->on || v->env == 0 || (v->vol_l == 0 && v->vol_r == 0)) {
        // Silent: keep its place in the sample without mixing it.
        if (v->on && !v->noise_src && !v->lp_noise) {
          v->pos += v->step * count;
          while (v->pos >= v->end) {
            if (!v->loop_back) { v->on = false; break; }
            v->pos -= v->loop_back;
          }
        }
        continue;
      }
      MixVoice(v, mix, count, noise);
    }
  }
}

void MixerOutput(int8_t *left, int8_t *right) {
  MixerState *d = &s_dsp;
  if (d->flg & 0x40) {  // muted
    memset(left, 0, kMixSamplesPerFrame);
    memset(right, 0, kMixSamplesPerFrame);
    return;
  }
  int ml = d->mvol_l, mr = d->mvol_r;
  for (int i = 0; i < kMixSamplesPerFrame; i++) {
    // The voices' sum is the DSP's at << 10; the master volume is >> 7; the
    // 8-bit output is the 16-bit one >> 8.
    // 1.5x: the 8-bit output has headroom to spare, and no echo adds to it.
    int32_t l = (((s_mix[i * 2] >> 10) * ml) * 3) >> 16;
    int32_t r = (((s_mix[i * 2 + 1] >> 10) * mr) * 3) >> 16;
    if (l > 127) l = 127; else if (l < -128) l = -128;
    if (r > 127) r = 127; else if (r < -128) r = -128;
    left[i] = l, right[i] = r;
  }
}
