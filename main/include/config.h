#pragma once
#include <stdint.h>
#include <stddef.h>

// Single source of truth for the pipeline geometry. Every derived size is a
// static_assert'd constexpr so a typo fails the build, not the demo.

namespace cfg {

constexpr int kSampleRate   = 16000;          // Hz, mono
constexpr int kBitsPerSample = 16;

// --- framing ---------------------------------------------------------------
constexpr int kFrameMs      = 20;             // VAD / hop interval
constexpr int kFrameSamples = kSampleRate * kFrameMs / 1000;      // 320
constexpr int kWindowMs     = 30;             // log-mel analysis window
constexpr int kWindowSamples= kSampleRate * kWindowMs / 1000;     // 480
constexpr int kFramesPerSec = 1000 / kFrameMs;                    // 50
constexpr int kMelBins      = 40;

static_assert(kFrameSamples  == 320, "20 ms @ 16 kHz must be 320 samples");
static_assert(kWindowSamples == 480, "30 ms @ 16 kHz must be 480 samples");
static_assert(kFramesPerSec  == 50,  "20 ms hop must give 50 fps");

// --- pre-roll ring buffer --------------------------------------------------
// 1.5 s: long enough to hold the wake word plus the run-up to it, so the
// trigger reads audio that already happened instead of starting a recording.
constexpr int kRingMs      = 1500;
constexpr int kRingSamples = kSampleRate * kRingMs / 1000;        // 24000
constexpr size_t kRingBytes = (size_t)kRingSamples * sizeof(int16_t);  // 48000

static_assert(kRingSamples == 24000, "1.5 s @ 16 kHz must be 24000 samples");
static_assert(kRingBytes   == 48000, "24000 int16 must be 48000 bytes");

// --- targets (for PASS/FAIL reporting only) --------------------------------
constexpr size_t kRamTargetBytes  = 256u * 1024;
constexpr int    kHandoffTargetMs = 200;

}  // namespace cfg
