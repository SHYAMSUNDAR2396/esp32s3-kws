#pragma once
#include <stdint.h>
#include <stddef.h>

// Pre-warmed uplink + simulated server.
//
// *** THIS IS A SOFTWARE SIMULATION OF THE TRANSPORT, NOT A NETWORK TEST. ***
// There is no real Wi-Fi association, no real TLS handshake, no real socket
// and no real Raspberry Pi. Wokwi's networking cannot give trustworthy
// millisecond timings, so transport latency here measures only the CPU cost
// of framing and the codec -- NOT time on the wire.
// Real RTT, TLS handshake cost and Wi-Fi jitter REQUIRE REAL HARDWARE.

namespace net {

struct Timestamps {
  int64_t keyword_us;      // KWS became confident
  int64_t preroll_us;      // ring buffer extracted
  int64_t encoded_us;      // ADPCM encode finished
  int64_t sent_us;         // handed to the (simulated) socket
  int64_t received_us;     // simulated server accepted it
  int64_t partial_us;      // simulated server produced a first partial
};

// "Pre-warmed": the connection is established at boot and held open, so the
// wake-word path never pays for DNS + TCP + TLS. That is the entire point --
// a cold TLS handshake is typically 100-300 ms on Wi-Fi and would blow the
// 200 ms budget on its own.
bool  connect_prewarmed();
bool  is_connected();
int64_t warmup_cost_us();

void  begin_utterance();
// Send one ~20 ms ADPCM chunk. Returns simulated server receive timestamp.
int64_t send_chunk(const uint8_t *data, size_t len);
void  end_utterance();

// --- simulated server side ---
size_t server_frames_received();
size_t server_bytes_received();
size_t server_samples_decoded();
const char *server_partial_transcript();   // canned text, NOT ASR

void  server_reset();

}  // namespace net
