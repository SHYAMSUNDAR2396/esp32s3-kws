#include "vad.h"

VadResult Vad::process(const int16_t *f) {
  // Energy: sum of squares in int64 (320 * 32768^2 overflows int32), then
  // divided down to a per-sample mean square so the threshold is frame-length
  // independent.
  int64_t sq = 0;
  uint16_t z = 0;
  int16_t prev = f[0];
  for (int i = 0; i < cfg::kFrameSamples; i++) {
    int32_t s = f[i];
    sq += (int64_t)s * s;
    if ((s < 0) != (prev < 0)) z++;
    prev = f[i];
  }
  uint32_t energy = (uint32_t)(sq / cfg::kFrameSamples);

  bool raw = energy >= cfg_.energy_thresh && z >= cfg_.zcr_min && z <= cfg_.zcr_max;

  // Hangover: speech has short internal pauses (stop consonants). Cutting the
  // cascade off mid-word would truncate the very audio the ring buffer exists
  // to preserve.
  if (raw) hang_ = cfg_.hangover;
  else if (hang_) hang_--;

  bool active = raw || hang_ > 0;
  n_++; if (active) acc_++;
  return {energy, z, active, raw};
}
