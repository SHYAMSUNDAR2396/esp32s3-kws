#include "network.h"
#include "adpcm.h"
#include "config.h"
#include <stdio.h>
#include <string.h>
#include "esp_timer.h"

namespace net {
namespace {

bool     g_connected = false;
int64_t  g_warmup_us = 0;

adpcm::State g_server_dec;
size_t   g_frames = 0, g_bytes = 0, g_samples = 0;
int      g_partial_idx = 0;

// Canned strings. These are NOT speech recognition output. Integrating real
// sherpa-onnx would be a separate phase with a real ASR runtime on the Pi.
const char *kPartials[] = {
  "what is",
  "what is the",
  "what is the fuel",
  "what is the fuel level",
};
constexpr int kNumPartials = sizeof(kPartials) / sizeof(kPartials[0]);

int16_t g_decode_scratch[cfg::kFrameSamples * 2];

}  // namespace

bool connect_prewarmed() {
  // Simulates: Wi-Fi assoc -> DHCP -> DNS -> TCP -> TLS -> WS upgrade, all at
  // boot. No cost is invented here; we record what the (empty) simulation
  // actually took so the number is honest about being ~0.
  int64_t t0 = esp_timer_get_time();
  g_connected = true;
  g_warmup_us = esp_timer_get_time() - t0;
  server_reset();
  return true;
}

bool is_connected() { return g_connected; }
int64_t warmup_cost_us() { return g_warmup_us; }

void begin_utterance() { server_reset(); }

int64_t send_chunk(const uint8_t *data, size_t len) {
  if (!g_connected) return -1;
  // Simulated server accepts immediately and decodes. The only real work is
  // the ADPCM decode, so the timestamp reflects CPU, not network transit.
  g_frames++;
  g_bytes += len;
  size_t n = adpcm::decode(g_server_dec, data, len, g_decode_scratch);
  g_samples += n;
  if (g_frames % 8 == 0 && g_partial_idx < kNumPartials) g_partial_idx++;
  return esp_timer_get_time();
}

void end_utterance() {}

size_t server_frames_received()  { return g_frames; }
size_t server_bytes_received()   { return g_bytes; }
size_t server_samples_decoded()  { return g_samples; }

const char *server_partial_transcript() {
  if (g_partial_idx <= 0) return "";
  return kPartials[g_partial_idx - 1];
}

void server_reset() {
  g_server_dec = adpcm::State();
  g_frames = 0; g_bytes = 0; g_samples = 0; g_partial_idx = 0;
}

}  // namespace net
