#pragma once
#include <stdint.h>
#include <stddef.h>

// ============================================================================
// SIMULATION INPUT -- NOT A MICROPHONE.
// Deterministic synthetic PCM used to exercise the VAD/KWS/uplink path. It
// proves the ARCHITECTURE moves the right bytes at the right times. It proves
// NOTHING about acoustic accuracy, mic AGC, I2S DMA jitter, or real-world
// false accepts. Those require a physical ESP32-S3 with a real microphone.
// Kept deliberately separate from the pipeline so it can be swapped for I2S.
// ============================================================================

enum class AudioCase : uint8_t {
  Silence,      // near-zero amplitude + dither
  Noise,        // broadband low/moderate noise floor
  Speech,       // voiced-ish: pitch + formants + noise, amplitude modulated
  WakeWord,     // a distinct deterministic signature (see note in .cpp)
  PostSpeech,   // speech again, different pitch -- the "query" after the word
};

class AudioSim {
 public:
  void reset(uint32_t seed = 12345u);
  // Fill one 20 ms frame (cfg::kFrameSamples) for the given case.
  void fill_frame(AudioCase c, int16_t *dst);

  // --- Case D: deterministic wake-word test sequence ---
  // silence, silence, noise, speech, WAKE, post-speech, silence
  // Advances one 20 ms frame per call; wraps forever.
  AudioCase scripted_next(int16_t *dst);
  uint32_t script_frame_index() const { return script_i_; }
  bool script_frame_is_wake() const { return last_was_wake_; }

 private:
  uint32_t rng_ = 12345u;
  uint32_t phase_ = 0;        // sample counter, drives oscillators
  uint32_t script_i_ = 0;
  bool last_was_wake_ = false;
  inline int32_t rnd();       // deterministic LCG, no libc rand()
};
