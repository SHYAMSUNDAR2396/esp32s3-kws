#include "audio_sim.h"
#include "config.h"
#include <math.h>

// Table oscillator instead of sinf(). Not an optimisation of the product --
// the synthetic source is scaffolding -- but sinf() made a 1000-frame cascade
// run exceed the Wokwi simulation budget. Phase is a Q32 accumulator so it
// wraps exactly and stays bit-identical across runs.
namespace {
constexpr int kSinBits = 10, kSinN = 1 << kSinBits;   // 1024 entries
float g_sin[kSinN];
bool  g_sin_ready = false;
void sin_table_init() {
  if (g_sin_ready) return;
  for (int i = 0; i < kSinN; i++) g_sin[i] = sinf(2.0f * (float)M_PI * i / kSinN);
  g_sin_ready = true;
}
}  // namespace

static inline float osc(uint32_t n, float hz) {
  // 2^32 / 16000 = 268435.456
  uint32_t k = (uint32_t)(hz * 268435.456f);
  return g_sin[(n * k) >> (32 - kSinBits)];
}

// Deterministic LCG (Numerical Recipes constants). Same seed -> same audio on
// every run, on every machine. Reproducibility matters more than spectral
// realism here: the cascade numbers must be comparable between runs.
inline int32_t AudioSim::rnd() {
  rng_ = rng_ * 1664525u + 1013904223u;
  return (int32_t)((rng_ >> 16) & 0xFFFF) - 32768;  // [-32768, 32767]
}

void AudioSim::reset(uint32_t seed) {
  sin_table_init();
  rng_ = seed; phase_ = 0; script_i_ = 0; last_was_wake_ = false;
}


void AudioSim::fill_frame(AudioCase c, int16_t *dst) {
  for (int i = 0; i < cfg::kFrameSamples; i++, phase_++) {
    float s = 0.0f;
    switch (c) {
      case AudioCase::Silence:
        // dither only: a real ADC never outputs a flat zero
        s = rnd() * 0.0008f;
        break;

      case AudioCase::Noise:
        s = rnd() * 0.030f;
        break;

      case AudioCase::Speech: {
        // F0 ~ 120 Hz + two formants, amplitude-modulated at a syllable rate.
        float env = 0.55f + 0.45f * osc(phase_, 5.0f);
        s = env * (7000.0f * osc(phase_, 120.0f)
                 + 3500.0f * osc(phase_, 700.0f)
                 + 1800.0f * osc(phase_, 1220.0f))
            + rnd() * 0.05f;
        break;
      }

      case AudioCase::WakeWord: {
        // NOTE: this is a synthetic SIGNATURE, not a recording of a spoken
        // wake word. It is a rising formant sweep with a higher zero-crossing
        // rate than the Speech case, so the downstream stages have something
        // separable to key on. Any detection score against this is a test of
        // the PLUMBING, never of wake-word accuracy.
        float t = (phase_ % 8000) / 8000.0f;      // 0.5 s sweep
        float env = 0.5f + 0.5f * osc(phase_, 3.0f);
        s = env * (6500.0f * osc(phase_, 150.0f + 40.0f * t)
                 + 4200.0f * osc(phase_, 900.0f + 1500.0f * t)
                 + 2600.0f * osc(phase_, 2400.0f + 900.0f * t))
            + rnd() * 0.06f;
        break;
      }

      case AudioCase::PostSpeech: {
        float env = 0.5f + 0.5f * osc(phase_, 6.5f);
        s = env * (6000.0f * osc(phase_, 165.0f)
                 + 3000.0f * osc(phase_, 820.0f)
                 + 1500.0f * osc(phase_, 1600.0f))
            + rnd() * 0.05f;
        break;
      }
    }
    if (s >  32767.0f) s =  32767.0f;
    if (s < -32768.0f) s = -32768.0f;
    dst[i] = (int16_t)s;
  }
}

// --- Case D script ----------------------------------------------------------
// Durations in 20 ms frames. Total 5.0 s, then repeats.
namespace {
struct Seg { AudioCase c; uint16_t frames; };
constexpr Seg kScript[] = {
  { AudioCase::Silence,    40 },  // 800 ms
  { AudioCase::Silence,    30 },  // 600 ms
  { AudioCase::Noise,      35 },  // 700 ms
  { AudioCase::Speech,     30 },  // 600 ms
  { AudioCase::WakeWord,   30 },  // 600 ms  <-- the trigger window
  { AudioCase::PostSpeech, 60 },  // 1200 ms
  { AudioCase::Silence,    25 },  // 500 ms
};
constexpr uint32_t kScriptFrames = 40+30+35+30+30+60+25;  // 250 frames = 5.0 s
}  // namespace

AudioCase AudioSim::scripted_next(int16_t *dst) {
  uint32_t pos = script_i_ % kScriptFrames, acc = 0;
  AudioCase c = AudioCase::Silence;
  for (const auto &seg : kScript) {
    if (pos < acc + seg.frames) { c = seg.c; break; }
    acc += seg.frames;
  }
  fill_frame(c, dst);
  last_was_wake_ = (c == AudioCase::WakeWord);
  script_i_++;
  return c;
}
