#include "features.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include "esp_dsp.h"

namespace feat {
namespace {

float g_hann[cfg::kWindowSamples];
// Complex interleaved buffer for esp-dsp: [re,im, re,im, ...]
float g_fft[kFftSize * 2];

// Sparse mel filterbank. Each of the 40 triangular filters touches only a
// contiguous span of FFT bins, so storing a dense 40x257 matrix (41 KB) would
// be wasteful on a part where we are counting kilobytes.
struct MelFilter { uint16_t start, count, offset; };
MelFilter g_mel[kMelBins];
float g_mel_w[kNumBins * 3];   // packed weights; spans overlap by ~2x
uint16_t g_mel_w_used = 0;
bool g_ready = false;

inline float hz_to_mel(float hz) { return 2595.0f * log10f(1.0f + hz / 700.0f); }
inline float mel_to_hz(float m)  { return 700.0f * (powf(10.0f, m / 2595.0f) - 1.0f); }

}  // namespace

size_t static_bytes() {
  return sizeof(g_hann) + sizeof(g_fft) + sizeof(g_mel) + sizeof(g_mel_w);
}

bool init() {
  if (g_ready) return true;

  // Periodic Hann.
  for (int i = 0; i < cfg::kWindowSamples; i++)
    g_hann[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / cfg::kWindowSamples);

  // Mel filterbank: 40 triangles equally spaced on the mel scale between
  // 20 Hz and Nyquist (8 kHz). Speech energy is concentrated well below
  // 8 kHz, and mel spacing puts the resolution where the formants are.
  const float lo_mel = hz_to_mel(20.0f), hi_mel = hz_to_mel(cfg::kSampleRate / 2.0f);
  float edge_hz[kMelBins + 2];
  for (int i = 0; i < kMelBins + 2; i++)
    edge_hz[i] = mel_to_hz(lo_mel + (hi_mel - lo_mel) * i / (kMelBins + 1));

  const float bin_hz = (float)cfg::kSampleRate / kFftSize;
  g_mel_w_used = 0;
  for (int m = 0; m < kMelBins; m++) {
    float f_lo = edge_hz[m], f_ctr = edge_hz[m + 1], f_hi = edge_hz[m + 2];
    int b_lo = (int)ceilf(f_lo / bin_hz), b_hi = (int)floorf(f_hi / bin_hz);
    if (b_lo < 0) b_lo = 0;
    if (b_hi > kNumBins - 1) b_hi = kNumBins - 1;
    if (b_hi < b_lo) b_hi = b_lo;

    g_mel[m].start  = (uint16_t)b_lo;
    g_mel[m].count  = (uint16_t)(b_hi - b_lo + 1);
    g_mel[m].offset = g_mel_w_used;
    for (int b = b_lo; b <= b_hi; b++) {
      float f = b * bin_hz, w;
      if (f <= f_ctr) w = (f_ctr > f_lo) ? (f - f_lo) / (f_ctr - f_lo) : 1.0f;
      else            w = (f_hi > f_ctr) ? (f_hi - f) / (f_hi - f_ctr) : 1.0f;
      if (w < 0) w = 0;
      if (g_mel_w_used >= sizeof(g_mel_w) / sizeof(float)) return false;
      g_mel_w[g_mel_w_used++] = w;
    }
  }

  if (dsps_fft2r_init_fc32(NULL, kFftSize) != ESP_OK) return false;
  g_ready = true;
  return true;
}

void log_mel(const int16_t *window, float *out) {
  // ponytail: real input packed into a full complex FFT (imag = 0). Costs ~2x
  // a proper real-FFT. Upgrade path if log-mel ever shows up hot in profiling:
  // dsps_fft2r_fc32 over N/2 + dsps_cplx2reC_fc32.
  for (int i = 0; i < cfg::kWindowSamples; i++) {
    g_fft[2 * i]     = (float)window[i] * (1.0f / 32768.0f) * g_hann[i];
    g_fft[2 * i + 1] = 0.0f;
  }
  memset(&g_fft[2 * cfg::kWindowSamples], 0,
         sizeof(float) * 2 * (kFftSize - cfg::kWindowSamples));   // zero-pad

  dsps_fft2r_fc32(g_fft, kFftSize);
  dsps_bit_rev_fc32(g_fft, kFftSize);

  // Power spectrum, then triangular mel integration, then natural log.
  for (int m = 0; m < kMelBins; m++) {
    const MelFilter &f = g_mel[m];
    float acc = 0.0f;
    for (int k = 0; k < f.count; k++) {
      int b = f.start + k;
      float re = g_fft[2 * b], im = g_fft[2 * b + 1];
      acc += (re * re + im * im) * g_mel_w[f.offset + k];
    }
    out[m] = logf(acc + 1e-10f);   // floor keeps log finite on digital silence
  }
}

}  // namespace feat
