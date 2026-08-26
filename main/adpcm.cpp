#include "adpcm.h"
#include <math.h>
#include <stdio.h>

namespace adpcm {
namespace {

const int8_t kIndexTable[16] = {
  -1, -1, -1, -1, 2, 4, 6, 8,
  -1, -1, -1, -1, 2, 4, 6, 8
};

const int16_t kStepTable[89] = {
      7,     8,     9,    10,    11,    12,    13,    14,    16,    17,
     19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
     50,    55,    60,    66,    73,    80,    88,    97,   107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
    337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
    876,   963,  1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
   2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
   5894,  6484,  7132,  7845,  8630,  9493, 10442, 11487, 12635, 13899,
  15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

inline uint8_t encode_sample(State &st, int16_t sample) {
  int step = kStepTable[st.index];
  int diff = sample - st.predictor;
  uint8_t code = 0;
  if (diff < 0) { code = 8; diff = -diff; }

  int delta = step >> 3;
  if (diff >= step)     { code |= 4; diff -= step;     delta += step; }
  step >>= 1;
  if (diff >= step)     { code |= 2; diff -= step;     delta += step; }
  step >>= 1;
  if (diff >= step)     { code |= 1;                   delta += step; }

  st.predictor += (code & 8) ? -delta : delta;
  if (st.predictor >  32767) st.predictor =  32767;
  if (st.predictor < -32768) st.predictor = -32768;

  st.index += kIndexTable[code];
  if (st.index < 0)  st.index = 0;
  if (st.index > 88) st.index = 88;
  return code;
}

inline int16_t decode_sample(State &st, uint8_t code) {
  int step = kStepTable[st.index];
  int delta = step >> 3;
  if (code & 4) delta += step;
  if (code & 2) delta += step >> 1;
  if (code & 1) delta += step >> 2;

  st.predictor += (code & 8) ? -delta : delta;
  if (st.predictor >  32767) st.predictor =  32767;
  if (st.predictor < -32768) st.predictor = -32768;

  st.index += kIndexTable[code];
  if (st.index < 0)  st.index = 0;
  if (st.index > 88) st.index = 88;
  return (int16_t)st.predictor;
}

}  // namespace

size_t encode(State &st, const int16_t *pcm, size_t n, uint8_t *out) {
  size_t w = 0;
  for (size_t i = 0; i < n; i += 2) {
    uint8_t lo = encode_sample(st, pcm[i]);
    uint8_t hi = (i + 1 < n) ? encode_sample(st, pcm[i + 1]) : 0;
    out[w++] = (uint8_t)(lo | (hi << 4));
  }
  return w;
}

size_t decode(State &st, const uint8_t *in, size_t n_bytes, int16_t *pcm) {
  size_t w = 0;
  for (size_t i = 0; i < n_bytes; i++) {
    pcm[w++] = decode_sample(st, in[i] & 0x0F);
    pcm[w++] = decode_sample(st, (in[i] >> 4) & 0x0F);
  }
  return w;
}

bool self_check(float *rms_err_out) {
  // A swept sine exercises the step-size adaptation in both directions.
  constexpr int N = 512;
  static int16_t src[N], back[N];
  static uint8_t enc[N / 2 + 1];
  for (int i = 0; i < N; i++)
    src[i] = (int16_t)(12000.0 * sin(2.0 * M_PI * i * (1.0 + i / 256.0) / 64.0));

  State e, d;
  size_t nb = encode(e, src, N, enc);
  size_t ns = decode(d, enc, nb, back);
  if (nb != N / 2 || ns != (size_t)N) return false;

  double acc = 0;
  for (int i = 0; i < N; i++) {
    double err = (double)src[i] - back[i];
    acc += err * err;
  }
  float rms = (float)sqrt(acc / N);
  if (rms_err_out) *rms_err_out = rms;

  // IMA ADPCM on a 12000-amplitude signal should stay well under ~5% RMS.
  // This bound catches a broken step table or nibble packing, which would
  // produce garbage an order of magnitude worse.
  return rms < 600.0f;
}

}  // namespace adpcm
