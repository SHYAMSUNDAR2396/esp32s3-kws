#pragma once
#include <stdint.h>
#include "config.h"

// Stage 1 of the cascade. Runs on EVERY 20 ms frame, so it must be cheap:
// integer energy + zero-crossing count, no float, no transforms.
// Its only job is to decide whether the expensive DS-CNN gets to run.
//
// THRESHOLDS ARE TUNABLE PARAMETERS, NOT VALIDATED CONSTANTS. They are set
// from the measured statistics of the SYNTHETIC audio in audio_sim.cpp. On
// real hardware with a real microphone they will be different and MUST be
// re-derived. REQUIRES REAL ESP32-S3 VALIDATION.
struct VadConfig {
  uint32_t energy_thresh = 0;   // mean square per sample; 0 = auto from noise floor
  uint16_t zcr_min       = 0;   // crossings per 320-sample frame
  uint16_t zcr_max       = 250;
  uint8_t  hangover      = 8;   // frames to stay "active" after energy drops
};

struct VadResult {
  uint32_t energy;      // mean square per sample (integer)
  uint16_t zcr;         // zero crossings in the frame
  bool     active;      // final decision, incl. hangover
  bool     raw_active;  // decision before hangover
};

class Vad {
 public:
  void init(const VadConfig &c) { cfg_ = c; hang_ = 0; }
  VadResult process(const int16_t *frame);   // cfg::kFrameSamples samples

  // Counters for the cascade report.
  uint32_t frames() const { return n_; }
  uint32_t accepted() const { return acc_; }
  uint32_t rejected() const { return n_ - acc_; }
  const VadConfig &config() const { return cfg_; }

 private:
  VadConfig cfg_;
  uint8_t hang_ = 0;
  uint32_t n_ = 0, acc_ = 0;
};
