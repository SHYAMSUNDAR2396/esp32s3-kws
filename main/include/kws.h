#pragma once
#include <stdint.h>
#include <stddef.h>
#include "config.h"

// Stage 2 of the cascade: INT8 DS-CNN over a sliding log-mel context window.
//
// *** THE MODEL IS AN UNTRAINED PLACEHOLDER (random weights). ***
// Arena size, model footprint, tensor shapes and inference time are REAL
// measurements of the runtime. The SCORES ARE MEANINGLESS. No accuracy,
// false-accept or false-reject figure may be derived from this.
//
// ponytail: sliding-window streaming, not stateful-cached streaming. Each
// inference recomputes the full 500 ms context. Upgrade path if inference
// shows up hot: cache per-layer activations across hops (true streaming
// DS-CNN), which trades ~arena for ~compute.

namespace kws {

constexpr int kContextFrames = 25;             // 25 x 20 ms = 500 ms
constexpr int kNumClasses    = 2;              // {not-wake, wake}

struct Decision {
  float raw_score;        // model P(wake) this frame
  float smoothed;         // moving average
  bool  detected;         // smoothed > threshold && not in cooldown
  bool  in_cooldown;
};

bool   init();                 // builds interpreter, allocates arena
size_t arena_used();           // MEASURED bytes actually used by TFLM
size_t arena_capacity();
size_t model_bytes();
void   tensor_info();          // prints measured input/output shapes + scales

// Push one log-mel frame into the context window. Returns false until the
// window has filled (the first 25 frames after boot cannot be classified).
bool push_frame(const float *mel /* [40] */);

// Run the DS-CNN on the current context window. Only call when VAD passed.
float infer();                 // returns P(wake) -- MEANINGLESS, untrained

// Phase 8 decision logic.
struct DecisionConfig {
  float    threshold      = 0.95f;  // conservative
  int      smooth_window  = 5;      // frames of moving average
  uint32_t cooldown_ms    = 1500;   // refractory after a fire
};
void     configure(const DecisionConfig &c);
Decision decide(float raw_score, int64_t now_us);
void     reset_decision();

}  // namespace kws
