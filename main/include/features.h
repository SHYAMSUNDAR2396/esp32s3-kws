#pragma once
#include <stdint.h>
#include "config.h"

// Log-mel front end.
//   30 ms window (480 samples) -> 512-pt FFT -> 40 mel bins -> log
//   advanced every 20 ms hop  -> 50 frames/s -> 2000 feature values/s
// FFT is esp-dsp (espressif/esp-dsp), i.e. the Xtensa-optimised routine you
// would actually ship on an S3, so the timing is representative rather than
// a naive-DFT worst case.

namespace feat {

constexpr int kFftSize  = 512;                 // next pow2 >= 480
constexpr int kNumBins  = kFftSize / 2 + 1;    // 257 usable magnitude bins
constexpr int kMelBins  = cfg::kMelBins;       // 40

bool init();                 // builds Hann window + mel filterbank, inits esp-dsp
size_t static_bytes();       // measured footprint of the tables

// Compute one log-mel frame from `kWindowSamples` PCM samples.
void log_mel(const int16_t *window, float *out /* [kMelBins] */);

}  // namespace feat
