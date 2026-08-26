#pragma once
#include <stdint.h>
#include <stddef.h>

// IMA ADPCM (DVI4). 16-bit PCM -> 4-bit nibbles = fixed 4:1 compression.
//   16 kHz x 16 bit = 256 kbps  ->  16 kHz x 4 bit = 64 kbps
// Chosen over Opus deliberately: Opus would compress far harder but costs an
// order of magnitude more CPU and adds algorithmic delay, and this link only
// has to cross a LAN to the Pi, not the internet. Bandwidth is not the
// constraint here; latency and CPU are.

namespace adpcm {

// Encoder/decoder state must persist across chunks -- IMA ADPCM is a
// predictive codec, so resetting per chunk would corrupt every chunk boundary.
struct State { int32_t predictor = 0; int8_t index = 0; };

// Encodes n_samples PCM into ceil(n/2) bytes. Returns bytes written.
size_t encode(State &st, const int16_t *pcm, size_t n_samples, uint8_t *out);
size_t decode(State &st, const uint8_t *in, size_t n_bytes, int16_t *pcm);

// Round-trip check: IMA ADPCM is lossy, so this asserts bounded error, not
// equality. Returns RMS error over a synthetic sweep.
bool self_check(float *rms_err_out);

}  // namespace adpcm
