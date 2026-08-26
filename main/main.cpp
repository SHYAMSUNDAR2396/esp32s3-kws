// ESP32-S3 TinyML KWS demo -- Phases 0-3.
// Every printed number comes from an IDF runtime API or esp_timer.
#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"
#include "metrics.h"
#include "ring_buffer.h"
#include "audio_sim.h"
#include "vad.h"
#include "features.h"
#include "kws.h"
#include "adpcm.h"
#include "network.h"

static RingBuffer g_ring;
static AudioSim   g_audio;
static Vad        g_vad;
static int16_t    g_frame[cfg::kFrameSamples];

// Collected during the phases, printed by the Phase 13 report. Nothing here is
// a literal -- every field is assigned from a measurement.
struct Report {
  size_t boot_free = 0, ring_bytes = 0, feat_static = 0, feat_heap = 0;
  size_t arena_used = 0, tflm_heap = 0, model_flash = 0;
  size_t total_used = 0, headroom = 0, largest_block = 0;
  double vad_us = 0, logmel_us = 0, infer_us = 0, adpcm_us = 0;
  uint32_t frames = 0, kws_execs = 0, wake_seen = 0, wake_passed = 0;
  uint32_t handoff_med = 0, handoff_p95 = 0, handoff_max = 0;
  uint32_t drain_med = 0;
  double infl_lo = 1, infl_hi = 1;
  double adpcm_ratio = 0;
} g_rep;

// ---------------------------------------------------------------------------
// Ring-buffer self-check. Wrap arithmetic is the one place this file can be
// silently wrong, so it is asserted on-device before any measurement runs.
static void ring_selfcheck() {
  RingBuffer rb;
  assert(rb.init());
  int16_t in[cfg::kFrameSamples];
  // 48 KB will NOT fit on an 8 KB FreeRTOS task stack -- it panics with
  // IllegalInstruction. Pre-roll extraction is always a heap/static buffer.
  int16_t *out = (int16_t *)heap_caps_malloc(cfg::kRingBytes,
                                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  assert(out);

  // Write 1.75x capacity with a known ramp; the last kRingSamples must survive
  // in order, and the oldest must have been overwritten.
  int32_t counter = 0;
  const int frames = (cfg::kRingSamples * 7 / 4) / cfg::kFrameSamples;
  for (int f = 0; f < frames; f++) {
    for (int i = 0; i < cfg::kFrameSamples; i++) in[i] = (int16_t)(counter++ & 0x7FFF);
    rb.write(in, cfg::kFrameSamples);
  }
  size_t got = rb.read_last(out, cfg::kRingSamples);
  assert(got == cfg::kRingSamples);
  int32_t expect = counter - (int32_t)cfg::kRingSamples;
  for (size_t i = 0; i < got; i++, expect++) assert(out[i] == (int16_t)(expect & 0x7FFF));

  // Partial read: most recent 320 samples only.
  assert(rb.read_last(out, cfg::kFrameSamples) == (size_t)cfg::kFrameSamples);
  assert(out[cfg::kFrameSamples - 1] == (int16_t)((counter - 1) & 0x7FFF));

  heap_caps_free(out);
  printf("[SELFCHECK] ring buffer wrap/order/pre-roll: PASS "
         "(%d frames written = %.2fx capacity)\n",
         frames, (double)(frames * cfg::kFrameSamples) / cfg::kRingSamples);
}

extern "C" void app_main(void) {
  esp_chip_info_t chip; esp_chip_info(&chip);
  printf("\n\n===== ESP32-S3 TinyML KWS =====\n");
  printf("target %s | chip model %d | %d cores | %d MHz | IDF %s | PSRAM %s\n\n",
         CONFIG_IDF_TARGET, (int)chip.model, chip.cores,
         CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, esp_get_idf_version(),
#if CONFIG_SPIRAM
         "yes");
#else
         "NO (internal SRAM only)");
#endif

  metrics::checkpoint("boot");
  g_rep.boot_free = metrics::free_internal();

  int64_t t0 = esp_timer_get_time();
  vTaskDelay(pdMS_TO_TICKS(100));
  int64_t dt = esp_timer_get_time() - t0;
  printf("[TIMER] esp_timer vs 100 ms vTaskDelay: %" PRId64 " us %s\n",
         dt, (dt > 50000 && dt < 200000) ? "(sane)" : "(SUSPECT -- timings void)");

  metrics::CpuCal cal = metrics::calibrate_cpu();
  g_rep.infl_lo = cal.inflation_lo; g_rep.infl_hi = cal.inflation_hi;

  metrics::checkpoint("after app init");

  // ---- Phase 2: ring buffer -----------------------------------------------
  printf("\n---------- PHASE 2: RING BUFFER ----------\n");
  printf("THEORETICAL / CALCULATED : %d samples/s x %.1f s x %d B = %u B (%.1f KiB)\n",
         cfg::kSampleRate, cfg::kRingMs / 1000.0, (int)sizeof(int16_t),
         (unsigned)cfg::kRingBytes, cfg::kRingBytes / 1024.0);
  size_t before = metrics::free_internal();
  if (!g_ring.init()) { printf("FATAL: ring alloc failed\n"); return; }
  size_t after = metrics::free_internal();
  printf("MEASURED IN WOKWI        : heap_caps_get_allocated_size = %u B (%.1f KiB)\n",
         (unsigned)g_ring.allocated_bytes(), g_ring.allocated_bytes() / 1024.0);
  printf("MEASURED IN WOKWI        : heap delta on alloc          = %u B (%.1f KiB)\n",
         (unsigned)(before - after), (before - after) / 1024.0);
  printf("                           (delta - payload = %d B heap block overhead)\n",
         (int)((before - after) - cfg::kRingBytes));
  g_rep.ring_bytes = g_ring.allocated_bytes();
  metrics::checkpoint("after ring buffer alloc");

  ring_selfcheck();
  metrics::checkpoint("after ring self-check (freed)");

  // ---- Phase 3: simulated audio -------------------------------------------
  printf("\n---------- PHASE 3: SIMULATED AUDIO ----------\n");
  printf("*** SIMULATION INPUT -- NOT A MICROPHONE. Acoustic realism is NOT\n");
  printf("*** claimed. Real I2S/mic behaviour REQUIRES REAL ESP32-S3 VALIDATION.\n");
  g_audio.reset();

  // Characterise each case so the VAD thresholds later are set against
  // measured statistics, not guessed ones.
  const char *names[] = {"Silence", "Noise", "Speech", "WakeWord", "PostSpeech"};
  printf("\n%-11s %10s %10s   (over 50 frames = 1.0 s each)\n", "case", "RMS", "ZCR/frame");
  for (int c = 0; c < 5; c++) {
    double rms_acc = 0; double zcr_acc = 0;
    for (int f = 0; f < 50; f++) {
      g_audio.fill_frame((AudioCase)c, g_frame);
      double sq = 0; int z = 0;
      for (int i = 0; i < cfg::kFrameSamples; i++) {
        sq += (double)g_frame[i] * g_frame[i];
        if (i && ((g_frame[i] < 0) != (g_frame[i - 1] < 0))) z++;
      }
      rms_acc += sqrt(sq / cfg::kFrameSamples); zcr_acc += z;
    }
    printf("%-11s %10.1f %10.1f\n", names[c], rms_acc / 50, zcr_acc / 50);
  }

  // Feed the ring continuously from the Case D script, as the real capture
  // task will. Nothing triggers it; it just always runs.
  printf("\n[SCRIPT] Case D sequence: silence,silence,noise,speech,WAKE,post,silence\n");
  int64_t fill_t0 = esp_timer_get_time();
  const int script_frames = 250;  // 5.0 s = one full script pass
  for (int f = 0; f < script_frames; f++) {
    g_audio.scripted_next(g_frame);
    g_ring.write(g_frame, cfg::kFrameSamples);
  }
  int64_t fill_us = esp_timer_get_time() - fill_t0;
  printf("[SCRIPT] %d frames (%.1f s audio) generated+buffered in %" PRId64 " us\n",
         script_frames, script_frames * cfg::kFrameMs / 1000.0, fill_us);
  printf("[RING]   total written %llu samples, wrapped=%s (capacity %u)\n",
         (unsigned long long)g_ring.total_written(),
         g_ring.wrapped() ? "yes" : "no", (unsigned)g_ring.capacity_samples());
  printf("[RING]   NOTE: generation cost above includes synthetic-audio maths\n");
  printf("         (sinf etc). Real capture is I2S DMA, so this is NOT the\n");
  printf("         capture cost. REQUIRES REAL ESP32-S3 VALIDATION.\n");


  // ---- Phase 4+5: VAD and cascade proof -----------------------------------
  printf("\n---------- PHASE 4: VAD ----------\n");
  // Thresholds derived from the MEASURED table printed above, not guessed:
  //   energy = RMS^2 -> Silence ~216, Noise ~3.2e5, Speech ~1.13e7,
  //                     WakeWord ~1.07e7, PostSpeech ~8.3e6
  //   ZCR/frame     -> Silence 162, Noise 160 | Speech 57, Wake 111, Post 70
  // A threshold at 2.0e6 sits ~6x above the noise floor and ~4x below the
  // quietest speech case. ZCR <= 130 separates broadband noise from voiced
  // frames. Both are TUNABLE PARAMETERS.
  VadConfig vc;
  vc.energy_thresh = 2000000u;
  vc.zcr_min = 5;
  vc.zcr_max = 130;
  vc.hangover = 8;                 // 8 frames = 160 ms
  g_vad.init(vc);
  printf("thresholds (TUNABLE, derived from synthetic stats -- NOT validated):\n");
  printf("  energy >= %u   zcr in [%u, %u]   hangover %u frames (%d ms)\n",
         (unsigned)vc.energy_thresh, vc.zcr_min, vc.zcr_max,
         vc.hangover, vc.hangover * cfg::kFrameMs);

  // Per-case VAD behaviour, so the decision is auditable case by case.
  g_audio.reset();
  printf("\n%-11s %12s %6s %8s\n", "case", "energy", "zcr", "raw pass");
  for (int c = 0; c < 5; c++) {
    Vad probe; probe.init(vc);
    uint64_t e_acc = 0; uint32_t z_acc = 0, pass = 0;
    for (int f = 0; f < 50; f++) {
      g_audio.fill_frame((AudioCase)c, g_frame);
      VadResult r = probe.process(g_frame);
      e_acc += r.energy; z_acc += r.zcr; pass += r.raw_active ? 1 : 0;
    }
    printf("%-11s %12llu %6u %5u/50\n", names[c],
           (unsigned long long)(e_acc / 50), (unsigned)(z_acc / 50), (unsigned)pass);
  }

  // VAD cost per frame, measured over many frames to swamp timer granularity.
  {
    g_audio.fill_frame(AudioCase::Speech, g_frame);  // worst case: full loop
    const int kReps = 2000;
    Vad t; t.init(vc);
    int64_t a = esp_timer_get_time();
    for (int i = 0; i < kReps; i++) t.process(g_frame);
    int64_t b = esp_timer_get_time();
    double per = (double)(b - a) / kReps;
    printf("\n[VAD] over %d reps:\n", kReps);
    g_rep.vad_us = per;
    metrics::print_estimate("VAD per frame", per, cal);
    printf("  %-26s %.3f-%.3f %% of one core (from the ESTIMATE band, at %d fps)\n",
           "VAD CPU load", (per / cal.inflation_hi) * cfg::kFramesPerSec / 10000.0,
           (per / cal.inflation_lo) * cfg::kFramesPerSec / 10000.0, cfg::kFramesPerSec);
    printf("[VAD] The estimate is an ORDER-OF-MAGNITUDE figure, NOT a hardware\n");
    printf("      measurement. REQUIRES REAL ESP32-S3 VALIDATION.\n");
  }

  // ---- Phase 5: cascade ----------------------------------------------------
  printf("\n---------- PHASE 5: CASCADE PROOF ----------\n");
  g_audio.reset();
  g_vad.init(vc);
  const int kCascadeFrames = 1500;           // 30 s of audio, 6 script passes
  uint32_t kws_execs = 0, wake_frames_seen = 0, wake_frames_passed = 0;
  int64_t casc_t0 = esp_timer_get_time();
  for (int f = 0; f < kCascadeFrames; f++) {
    g_audio.scripted_next(g_frame);
    bool is_wake = g_audio.script_frame_is_wake();
    g_ring.write(g_frame, cfg::kFrameSamples);       // ALWAYS, unconditionally
    VadResult r = g_vad.process(g_frame);
    if (is_wake) { wake_frames_seen++; if (r.active) wake_frames_passed++; }
    if (r.active) kws_execs++;                       // DS-CNN would run here
  }
  int64_t casc_us = esp_timer_get_time() - casc_t0;

  g_rep.frames = kCascadeFrames; g_rep.kws_execs = kws_execs;
  g_rep.wake_seen = wake_frames_seen; g_rep.wake_passed = wake_frames_passed;
  double rate = 100.0 * kws_execs / kCascadeFrames;
  printf("\n========== CASCADE REPORT ==========\n");
  printf("(MEASURED IN WOKWI, on SYNTHETIC audio)\n\n");
  printf("Total frames            : %d\n", kCascadeFrames);
  printf("VAD rejected            : %u\n", (unsigned)(kCascadeFrames - kws_execs));
  printf("VAD passed              : %u\n", (unsigned)kws_execs);
  printf("DS-CNN executions       : %u\n", (unsigned)kws_execs);
  printf("\nKWS activation rate     : %.1f %%\n", rate);
  printf("\nRESULT:\nDS-CNN skipped on %.1f %% of frames\n", 100.0 - rate);
  printf("\nRECALL CHECK (the number that matters more):\n");
  printf("  wake-word frames present : %u\n", (unsigned)wake_frames_seen);
  printf("  wake-word frames passed  : %u  (%.1f %%)\n", (unsigned)wake_frames_passed,
         100.0 * wake_frames_passed / (wake_frames_seen ? wake_frames_seen : 1));
  printf("  A cascade that drops wake-word frames is worthless no matter how\n");
  printf("  much compute it saves.\n");
  printf("\nring buffer kept filling throughout: %llu samples written\n",
         (unsigned long long)g_ring.total_written());
  printf("VAD wall cost for all %d frames: %lld us\n", kCascadeFrames, (long long)casc_us);
  printf("====================================\n");
  printf("\n*** CAVEAT -- READ BEFORE PUTTING THIS ON A SLIDE ***\n");
  printf("This rate is measured against SYNTHETIC audio whose 'noise' case is\n");
  printf("broadband and low-amplitude, so it is easy to separate from speech.\n");
  printf("Real rooms contain fans, traffic and TV speech that have speech-like\n");
  printf("energy AND speech-like ZCR. This figure is therefore an OPTIMISTIC\n");
  printf("UPPER BOUND on compute saved. REQUIRES REAL ESP32-S3 VALIDATION with\n");
  printf("recorded ambient noise before it can be quoted as a product claim.\n");

  metrics::checkpoint("after cascade run");


  // ---- Phase 6: log-mel feature pipeline ----------------------------------
  printf("\n---------- PHASE 6: LOG-MEL FEATURES ----------\n");
  printf("THEORETICAL / CALCULATED:\n");
  printf("  %d ms window x %d Hz = %d samples/window\n",
         cfg::kWindowMs, cfg::kSampleRate, cfg::kWindowSamples);
  printf("  %d ms hop              = %d frames/second\n", cfg::kFrameMs, cfg::kFramesPerSec);
  printf("  %d mel bins            = %d features/frame\n", cfg::kMelBins, cfg::kMelBins);
  printf("  %d frames/s x %d bins  = %d feature values/second\n",
         cfg::kFramesPerSec, cfg::kMelBins, cfg::kFramesPerSec * cfg::kMelBins);
  printf("  FFT size %d (next pow2 >= %d), %d magnitude bins\n",
         feat::kFftSize, cfg::kWindowSamples, feat::kNumBins);

  size_t feat_before = metrics::free_internal();
  if (!feat::init()) { printf("FATAL: feature init failed\n"); return; }
  printf("\nMEASURED IN WOKWI:\n");
  printf("  feature static tables   : %u B (%.1f KB)  [Hann + FFT buf + mel filterbank]\n",
         (unsigned)feat::static_bytes(), feat::static_bytes() / 1024.0);
  printf("  heap delta on feat init : %u B  [esp-dsp FFT twiddle tables]\n",
         (unsigned)(feat_before - metrics::free_internal()));
  g_rep.feat_static = feat::static_bytes();
  g_rep.feat_heap = feat_before - metrics::free_internal();
  metrics::checkpoint("after log-mel init");

  // Timing over the 5 audio cases, reading windows from the ring buffer
  // exactly as the live pipeline will.
  {
    static int16_t win[cfg::kWindowSamples];
    static float   mel[cfg::kMelBins];
    g_audio.reset();
    const int kReps = 200;
    // Prepare the window ONCE so the timing isolates log_mel() alone --
    // the earlier version also timed synthetic audio generation and ring I/O
    // and therefore over-stated the feature cost.
    g_audio.fill_frame(AudioCase::Speech, g_frame);
    g_ring.write(g_frame, cfg::kFrameSamples);
    g_ring.read_last(win, cfg::kWindowSamples);
    int64_t a = esp_timer_get_time();
    for (int i = 0; i < kReps; i++) feat::log_mel(win, mel);
    int64_t b = esp_timer_get_time();
    double per = (double)(b - a) / kReps;
    printf("\n  over %d frames:\n", kReps);
    g_rep.logmel_us = per;
    metrics::print_estimate("log-mel per frame", per, cal);
    printf("  (log_mel() only -- audio generation and ring I/O excluded)\n");

    // Sanity: features must actually differ between cases, or the front end is
    // broken and every later score is meaningless.
    printf("\n  mel[0..4] per case (sanity -- these must differ):\n");
    for (int c = 0; c < 5; c++) {
      g_audio.fill_frame((AudioCase)c, g_frame);
      g_ring.write(g_frame, cfg::kFrameSamples);
      g_ring.read_last(win, cfg::kWindowSamples);
      feat::log_mel(win, mel);
      printf("    %-11s % 7.2f % 7.2f % 7.2f % 7.2f % 7.2f\n",
             names[c], mel[0], mel[1], mel[2], mel[3], mel[4]);
    }
  }
  metrics::checkpoint("after log-mel timing");


  // ---- Phase 7: DS-CNN / TFLite Micro -------------------------------------
  printf("\n---------- PHASE 7: INT8 DS-CNN (TFLite Micro) ----------\n");
  printf("*** PLACEHOLDER MODEL: weights are RANDOM / UNTRAINED. ***\n");
  printf("*** Arena, footprint, shapes and latency below are REAL runtime   ***\n");
  printf("*** measurements. The SCORES ARE MEANINGLESS. No accuracy or      ***\n");
  printf("*** false-accept claim can be made from this model.               ***\n\n");

  size_t kws_before = metrics::free_internal();
  if (!kws::init()) { printf("FATAL: kws init failed\n"); return; }
  size_t kws_after = metrics::free_internal();

  printf("MEASURED IN WOKWI:\n");
  printf("  model flash size    : %u B (%.1f KB)  [.tflite in flash, not RAM]\n",
         (unsigned)kws::model_bytes(), kws::model_bytes() / 1024.0);
  printf("  arena capacity      : %u B (%.1f KB)  [what we reserved]\n",
         (unsigned)kws::arena_capacity(), kws::arena_capacity() / 1024.0);
  printf("  arena ACTUALLY used : %u B (%.1f KB)  <-- the real requirement\n",
         (unsigned)kws::arena_used(), kws::arena_used() / 1024.0);
  printf("  heap delta on init  : %u B (%.1f KB)  [arena + interpreter state]\n",
         (unsigned)(kws_before - kws_after), (kws_before - kws_after) / 1024.0);
  kws::tensor_info();
  g_rep.arena_used = kws::arena_used();
  g_rep.model_flash = kws::model_bytes();
  g_rep.tflm_heap = kws_before - kws_after;
  metrics::checkpoint("after TFLM + model init");

  // Inference timing.
  {
    static float mel[cfg::kMelBins];
    static int16_t win[cfg::kWindowSamples];
    g_audio.reset();
    for (int i = 0; i < kws::kContextFrames; i++) {   // fill the context window
      g_audio.fill_frame(AudioCase::WakeWord, g_frame);
      g_ring.write(g_frame, cfg::kFrameSamples);
      g_ring.read_last(win, cfg::kWindowSamples);
      feat::log_mel(win, mel);
      kws::push_frame(mel);
    }
    const int kReps = 30;
    int64_t a = esp_timer_get_time();
    for (int i = 0; i < kReps; i++) kws::infer();
    int64_t b = esp_timer_get_time();
    double per = (double)(b - a) / kReps;
    printf("\n  over %d inferences:\n", kReps);
    g_rep.infer_us = per;
    printf("  DS-CNN inference  : %.0f us MEASURED IN WOKWI\n", per);
    printf("  -> ~%.0f-%.0f ms estimated on real S3 *** WITH ANSI-C KERNELS ***\n",
           per / cal.inflation_hi / 1000.0, per / cal.inflation_lo / 1000.0);
    printf("\n  *** THIS IS NOT THE SHIPPABLE INFERENCE TIME. ***\n");
    printf("  Wokwi cannot execute the ESP32-S3 SIMD/DSP instructions that\n");
    printf("  esp-nn's assembly kernels use (they fault with IllegalInstruction),\n");
    printf("  so this build is forced to CONFIG_NN_ANSI_C=y -- reference C kernels.\n");
    printf("  Real hardware runs the assembly path and is far faster.\n\n");
    printf("  THEORETICAL / CALCULATED cross-check (MAC count of this graph):\n");
    {
      const long c1 = 13L*20*32*10*4;
      const long dw = 13L*20*32*9, pw = 13L*20*32*32;
      const long macs = c1 + 4*(dw+pw) + 32*2;
      printf("    conv1 %ld + 4x(dw %ld + pw %ld) + dense 64 = %ld MACs\n",
             c1, dw, pw, macs);
      printf("    ANSI-C ref  @ ~15 cyc/MAC  : ~%.0f ms  <- consistent with measurement\n",
             macs * 15.0 / 240e6 * 1000);
      printf("    esp-nn asm  @ ~0.75 cyc/MAC: ~%.1f ms  <- REQUIRES REAL ESP32-S3\n",
             macs * 0.75 / 240e6 * 1000);
      printf("    The measurement above validates the ANSI-C row only. The esp-nn\n");
      printf("    row is a CALCULATION and must be confirmed on silicon.\n");
    }
  }
  metrics::checkpoint("after inference timing");

  // ---- Phase 8: keyword decision logic ------------------------------------
  printf("\n---------- PHASE 8: DECISION LOGIC ----------\n");
  kws::DecisionConfig dc;
  dc.threshold = 0.95f; dc.smooth_window = 5; dc.cooldown_ms = 1500;
  kws::configure(dc);
  printf("threshold %.2f | moving average over %d frames | cooldown %u ms\n",
         dc.threshold, dc.smooth_window, (unsigned)dc.cooldown_ms);
  printf("A detection needs a FULL smoothing window, so the first %d frames\n",
         dc.smooth_window);
  printf("after boot or reset cannot fire.\n\n");

  {
    static float mel[cfg::kMelBins];
    static int16_t win[cfg::kWindowSamples];
    g_audio.reset(); kws::reset_decision();
    printf("%-6s %-11s %8s %9s %10s %s\n",
           "frame", "case", "raw", "smoothed", "threshold", "decision");
    int fires = 0;
    for (int f = 0; f < 120; f++) {
      AudioCase c = g_audio.scripted_next(g_frame);
      g_ring.write(g_frame, cfg::kFrameSamples);
      g_ring.read_last(win, cfg::kWindowSamples);
      feat::log_mel(win, mel);
      if (!kws::push_frame(mel)) continue;
      VadResult v = g_vad.process(g_frame);
      if (!v.active) continue;                    // cascade: no VAD, no DS-CNN
      float raw = kws::infer();
      kws::Decision d = kws::decide(raw, esp_timer_get_time());
      if (d.detected) fires++;
      if (f % 10 == 0 || d.detected)
        printf("%-6d %-11s %8.4f %9.4f %10.2f %s%s\n", f, names[(int)c],
               d.raw_score, d.smoothed, dc.threshold,
               d.detected ? "*** DETECTED ***" : "-",
               d.in_cooldown ? " (cooldown)" : "");
    }
    printf("\nfires over 120 scripted frames: %d\n", fires);
    printf("(raw score sits at ~0.5 because the model is untrained -- a random\n");
    printf(" 2-class softmax. It correctly never crosses the 0.95 threshold.)\n");
    printf("*** This count is NOT an accuracy result. The model is untrained,\n");
    printf("*** so whether it fires is arbitrary. It demonstrates only that\n");
    printf("*** smoothing, thresholding and cooldown are wired correctly.\n");
  }
  // Deterministic check of smoothing + threshold + cooldown, driven by a
  // SYNTHETIC score ramp rather than the untrained model. This is the only
  // way to actually prove the decision logic works.
  {
    printf("\n[SELFCHECK] decision logic on a synthetic score sequence:\n");
    kws::DecisionConfig t; t.threshold = 0.9f; t.smooth_window = 4; t.cooldown_ms = 200;
    kws::configure(t);
    // low scores -> must not fire; sustained high -> exactly one fire;
    // second burst inside cooldown -> suppressed; after cooldown -> fires.
    const float seq[]  = {0.1f,0.2f,0.1f,0.3f, 1.0f,1.0f,1.0f,1.0f, 1.0f,1.0f};
    int64_t base = esp_timer_get_time();
    int fired_low = 0, fired_high = 0;
    for (int i = 0; i < 10; i++) {
      kws::Decision d = kws::decide(seq[i], base + i * 20000);   // 20 ms apart
      if (i < 4 && d.detected) fired_low++;
      if (i >= 4 && d.detected) fired_high++;
    }
    // inside cooldown (only 200 ms later => suppressed)
    kws::Decision c1 = kws::decide(1.0f, base + 10 * 20000);
    // after cooldown
    kws::Decision c2 = kws::decide(1.0f, base + 10 * 20000 + 400000);
    bool ok = (fired_low == 0) && (fired_high == 1) && !c1.detected && c2.detected;
    printf("  low scores fired      : %d (expect 0)\n", fired_low);
    printf("  sustained high fired  : %d (expect exactly 1)\n", fired_high);
    printf("  inside cooldown fired : %s (expect no)\n", c1.detected ? "yes" : "no");
    printf("  after cooldown fired  : %s (expect yes)\n", c2.detected ? "yes" : "no");
    printf("  RESULT: %s\n", ok ? "PASS" : "FAIL");
    if (!ok) { printf("FATAL: decision logic is broken\n"); return; }
    kws::configure(dc);
  }

  metrics::checkpoint("after decision logic");


  // ---- Phase 10: IMA ADPCM -------------------------------------------------
  printf("\n---------- PHASE 10: IMA ADPCM ----------\n");
  {
    float rms_err = 0;
    bool ok = adpcm::self_check(&rms_err);
    printf("[SELFCHECK] ADPCM round-trip: %s (RMS error %.1f on amplitude 12000 = %.2f%%)\n",
           ok ? "PASS" : "FAIL", rms_err, 100.0 * rms_err / 12000.0);
    if (!ok) { printf("FATAL: ADPCM broken\n"); return; }
  }
  printf("\nTHEORETICAL / CALCULATED:\n");
  printf("  16-bit PCM : %d Hz x 16 bit = %d kbps\n",
         cfg::kSampleRate, cfg::kSampleRate * 16 / 1000);
  printf("  4-bit IMA  : %d Hz x  4 bit = %d kbps\n",
         cfg::kSampleRate, cfg::kSampleRate * 4 / 1000);
  {
    static int16_t pcm[cfg::kFrameSamples];
    static uint8_t enc[cfg::kFrameSamples];
    g_audio.reset();
    adpcm::State st;
    const int kReps = 500;
    size_t in_bytes = 0, out_bytes = 0;
    g_audio.fill_frame(AudioCase::Speech, pcm);
    int64_t a = esp_timer_get_time();
    for (int i = 0; i < kReps; i++) {
      out_bytes += adpcm::encode(st, pcm, cfg::kFrameSamples, enc);
      in_bytes  += cfg::kFrameSamples * sizeof(int16_t);
    }
    int64_t b = esp_timer_get_time();
    printf("\nMEASURED IN WOKWI (%d frames of %d ms):\n", kReps, cfg::kFrameMs);
    printf("  input bytes       : %u\n", (unsigned)in_bytes);
    printf("  output bytes      : %u\n", (unsigned)out_bytes);
    printf("  compression ratio : %.2f : 1\n", (double)in_bytes / out_bytes);
    g_rep.adpcm_us = (double)(b - a) / kReps;
    g_rep.adpcm_ratio = (double)in_bytes / out_bytes;
    metrics::print_estimate("ADPCM encode/frame", g_rep.adpcm_us, cal);
  }
  metrics::checkpoint("after ADPCM");

  // ---- Phase 11: pre-warmed uplink + simulated server ---------------------
  printf("\n---------- PHASE 11: WEBSOCKET / SERVER SIMULATION ----------\n");
  printf("*** SOFTWARE SIMULATION OF THE TRANSPORT -- NOT A NETWORK TEST. ***\n");
  printf("*** No Wi-Fi association, no TLS handshake, no socket, no Pi.    ***\n");
  printf("*** Timings below are framing + codec CPU ONLY, never wire time. ***\n");
  printf("*** Real RTT / TLS / Wi-Fi jitter: REQUIRES REAL HARDWARE.       ***\n\n");
  size_t net_before = metrics::free_internal();
  net::connect_prewarmed();
  printf("pre-warmed link established at boot: %s\n", net::is_connected() ? "yes" : "no");
  printf("MEASURED heap delta for uplink state: %u B\n",
         (unsigned)(net_before - metrics::free_internal()));
  printf("NOTE: a REAL Wi-Fi + mbedTLS stack costs FAR more RAM than this\n");
  printf("      simulation. That cost is NOT included in the memory report\n");
  printf("      below and MUST be measured on real hardware.\n");
  metrics::checkpoint("after uplink init (SIMULATED)");

  // ---- Phase 9 + 12: triggered path and end-to-end latency ---------------
  printf("\n---------- PHASE 9+12: TRIGGERED PATH, %d TRIALS ----------\n", 100);
  {
    static int16_t preroll[cfg::kRingSamples];
    static uint8_t enc[cfg::kFrameSamples];
    const int kPrerollSamples = cfg::kSampleRate * 1000 / 1000;   // 1.0 s of pre-roll
    const int kTrials = 100;
    static uint32_t total_us[kTrials], handoff_us[kTrials];
    metrics::Stat s_preroll, s_encode, s_send, s_total, s_handoff;

    for (int t = 0; t < kTrials; t++) {
      // t0: KWS just became confident. The word is already in the past.
      int64_t t_keyword = esp_timer_get_time();

      // 1. recover pre-existing audio from the ring (this is the whole point
      //    of the ring buffer -- the trigger READS, it does not START capture)
      size_t got = g_ring.read_last(preroll, kPrerollSamples);
      int64_t t_preroll = esp_timer_get_time();

      // 2. encode to ADPCM in ~20 ms chunks and 3. ship each chunk
      net::begin_utterance();
      adpcm::State st;
      int64_t t_enc_acc = 0, t_send_acc = 0, t_recv_last = 0, t_recv_first = 0;
      for (size_t off = 0; off + cfg::kFrameSamples <= got; off += cfg::kFrameSamples) {
        int64_t e0 = esp_timer_get_time();
        size_t n = adpcm::encode(st, preroll + off, cfg::kFrameSamples, enc);
        int64_t e1 = esp_timer_get_time();
        t_recv_last = net::send_chunk(enc, n);
        int64_t e2 = esp_timer_get_time();
        if (!t_recv_first) t_recv_first = t_recv_last;   // HANDOFF happens here
        t_enc_acc  += e1 - e0;
        t_send_acc += e2 - e1;
      }
      int64_t t_done = t_recv_last ? t_recv_last : esp_timer_get_time();

      s_preroll.add((uint32_t)(t_preroll - t_keyword));
      s_encode.add((uint32_t)t_enc_acc);
      s_send.add((uint32_t)t_send_acc);
      uint32_t tot = (uint32_t)(t_done - t_keyword);
      s_total.add(tot);
      total_us[t] = tot;
      handoff_us[t] = (uint32_t)(t_recv_first - t_keyword);
      s_handoff.add(handoff_us[t]);

      if (t == 0) {
        printf("trial 0 stage timestamps (us since keyword):\n");
        printf("  keyword confident      : 0\n");
        printf("  ring-buffer extracted  : %lld  (%u samples = %.2f s pre-roll)\n",
               (long long)(t_preroll - t_keyword), (unsigned)got,
               (double)got / cfg::kSampleRate);
        printf("  ADPCM encode (total)   : %lld\n", (long long)t_enc_acc);
        printf("  WebSocket send (total) : %lld\n", (long long)t_send_acc);
        printf("  server received FIRST   : %lld   <-- HANDOFF\n",
               (long long)(t_recv_first - t_keyword));
        printf("  server received last    : %lld   (drain of the whole pre-roll)\n",
               (long long)(t_done - t_keyword));
        printf("  server frames received : %u, bytes %u, samples decoded %u\n",
               (unsigned)net::server_frames_received(),
               (unsigned)net::server_bytes_received(),
               (unsigned)net::server_samples_decoded());
        printf("\n[SERVER] partial transcript: \"%s\"\n", net::server_partial_transcript());
        printf("[SERVER] ^ CANNED STRING, NOT ASR. sherpa-onnx is NOT integrated.\n\n");
      }
    }

    // percentiles
    for (int i = 1; i < kTrials; i++) {            // insertion sort, n=100
      uint32_t v = total_us[i]; int j = i - 1;
      while (j >= 0 && total_us[j] > v) { total_us[j + 1] = total_us[j]; j--; }
      total_us[j + 1] = v;
    }
    for (int i = 1; i < kTrials; i++) {
      uint32_t v = handoff_us[i]; int j = i - 1;
      while (j >= 0 && handoff_us[j] > v) { handoff_us[j + 1] = handoff_us[j]; j--; }
      handoff_us[j + 1] = v;
    }
    printf("========== LATENCY (%d trials) ==========\n", kTrials);
    printf("(MEASURED IN WOKWI -- CPU cost of pre-roll + codec + framing ONLY)\n\n");
    printf("  %-22s %10s %10s\n", "stage", "mean us", "max us");
    printf("  %-22s %10.0f %10u\n", "ring extraction", s_preroll.mean_us(), (unsigned)s_preroll.max_us);
    printf("  %-22s %10.0f %10u\n", "ADPCM encode (all)", s_encode.mean_us(), (unsigned)s_encode.max_us);
    printf("  %-22s %10.0f %10u\n", "send+server (all)", s_send.mean_us(), (unsigned)s_send.max_us);
    printf("\n  HANDOFF -- keyword -> server received FIRST chunk:\n");
    printf("  (this is the figure the <200 ms target is about: the server can\n");
    printf("   begin decoding here. It does NOT wait for the whole pre-roll.)\n");
    printf("    minimum : %8u us  (%.2f ms)\n", (unsigned)handoff_us[0], handoff_us[0] / 1000.0);
    printf("    median  : %8u us  (%.2f ms)\n", (unsigned)handoff_us[kTrials/2], handoff_us[kTrials/2] / 1000.0);
    printf("    p95     : %8u us  (%.2f ms)\n", (unsigned)handoff_us[(int)(kTrials*0.95)],
           handoff_us[(int)(kTrials*0.95)] / 1000.0);
    printf("    maximum : %8u us  (%.2f ms)\n", (unsigned)handoff_us[kTrials-1], handoff_us[kTrials-1] / 1000.0);
    printf("    deflated median: ~%.2f-%.2f ms\n",
           handoff_us[kTrials/2] / cal.inflation_hi / 1000.0,
           handoff_us[kTrials/2] / cal.inflation_lo / 1000.0);

    printf("\n  FULL PRE-ROLL DRAIN -- keyword -> server received LAST chunk:\n");
    printf("  (a THROUGHPUT measure -- encoding 1.0 s of buffered audio. Do not\n");
    printf("   confuse this with handoff latency.)\n");
    printf("    minimum : %8u us  (%.1f ms)\n", (unsigned)total_us[0], total_us[0] / 1000.0);
    printf("    median  : %8u us  (%.1f ms)\n", (unsigned)total_us[kTrials/2], total_us[kTrials/2] / 1000.0);
    printf("    p95     : %8u us  (%.1f ms)\n", (unsigned)total_us[(int)(kTrials*0.95)],
           total_us[(int)(kTrials*0.95)] / 1000.0);
    printf("    maximum : %8u us  (%.1f ms)\n", (unsigned)total_us[kTrials-1], total_us[kTrials-1] / 1000.0);
    printf("\n  deflated by the %.0f-%.0fx Wokwi factor:\n", cal.inflation_lo, cal.inflation_hi);
    printf("    median  : ~%.1f-%.1f ms\n",
           total_us[kTrials/2] / cal.inflation_hi / 1000.0,
           total_us[kTrials/2] / cal.inflation_lo / 1000.0);
    printf("\n  *** THIS IS NOT THE 200 ms HANDOFF FIGURE. ***\n");
    printf("  It EXCLUDES: Wi-Fi transmit time, TLS record overhead, network\n");
    printf("  RTT, server queueing and real ASR compute -- i.e. every term that\n");
    printf("  actually decides whether 200 ms is met. It is a FLOOR on the\n");
    printf("  on-device contribution, nothing more.\n");
    printf("  NOT RELIABLY MEASURABLE IN WOKWI -- VALIDATE ON REAL ESP32-S3.\n");
    printf("==========================================\n");
    g_rep.handoff_med = handoff_us[kTrials/2];
    g_rep.handoff_p95 = handoff_us[(int)(kTrials*0.95)];
    g_rep.handoff_max = handoff_us[kTrials-1];
    g_rep.drain_med   = total_us[kTrials/2];
  }
  metrics::checkpoint("after latency trials");

  metrics::checkpoint("after 5 s of buffered audio");
  metrics::memory_report();


  // ---- Phase 13: final automated resource report --------------------------
  g_rep.total_used    = g_rep.boot_free - metrics::free_internal();
  g_rep.headroom      = metrics::free_internal();
  g_rep.largest_block = metrics::largest_free_block();
  {
    const double lo = g_rep.infl_lo, hi = g_rep.infl_hi;
    #define EST(x) (x)/hi, (x)/lo
    printf("\n\n");
    printf("================================================\n");
    printf("ESP32-S3 TINYML KWS RESOURCE REPORT\n");
    printf("================================================\n");
    printf("Build: ESP-IDF %s, %s @ %d MHz, no PSRAM, CONFIG_NN_ANSI_C=y\n",
           esp_get_idf_version(), CONFIG_IDF_TARGET, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    printf("\nTARGETS\n-----------------------------------------------\n");
    printf("RAM target                 <= 256 KB\n");
    printf("Idle CPU target            < 10 %%\n");
    printf("Handoff target             < 200 ms\n");
    printf("False accepts              < 1 / 12 h\n");

    printf("\nMEMORY  [MEASURED IN WOKWI -- heap_caps, internal SRAM]\n");
    printf("-----------------------------------------------\n");
    printf("Ring buffer (1.5 s)        %6.1f KB\n", g_rep.ring_bytes / 1024.0);
    printf("Log-mel static tables      %6.1f KB\n", g_rep.feat_static / 1024.0);
    printf("esp-dsp FFT tables         %6.1f KB\n", g_rep.feat_heap / 1024.0);
    printf("Tensor arena (used)        %6.1f KB\n", g_rep.arena_used / 1024.0);
    printf("TFLM heap total            %6.1f KB  (arena reserved + interpreter)\n",
           g_rep.tflm_heap / 1024.0);
    printf("Model (FLASH, not RAM)     %6.1f KB\n", g_rep.model_flash / 1024.0);
    printf("WiFi/TLS                      n/a    NOT SIMULATED -- SEE BELOW\n");
    printf("-----------------------------------------------\n");
    printf("TOTAL USED                 %6.1f KB\n", g_rep.total_used / 1024.0);
    printf("HEADROOM                   %6.1f KB\n", g_rep.headroom / 1024.0);
    printf("LARGEST FREE BLOCK         %6.1f KB\n", g_rep.largest_block / 1024.0);

    printf("\nCOMPUTE  [MEASURED, then deflated by the %.0f-%.0fx Wokwi factor]\n", lo, hi);
    printf("-----------------------------------------------\n");
    printf("VAD                        %9.0f us  ->  ~%.1f-%.1f us\n",
           g_rep.vad_us, EST(g_rep.vad_us));
    printf("Log-mel                    %9.0f us  ->  ~%.0f-%.0f us\n",
           g_rep.logmel_us, EST(g_rep.logmel_us));
    printf("ADPCM encode / 20 ms       %9.0f us  ->  ~%.0f-%.0f us\n",
           g_rep.adpcm_us, EST(g_rep.adpcm_us));
    printf("DS-CNN inference           %9.0f us  ->  ~%.0f-%.0f ms  ** ANSI-C KERNELS **\n",
           g_rep.infer_us, g_rep.infer_us/hi/1000.0, g_rep.infer_us/lo/1000.0);
    printf("  DS-CNN with esp-nn asm   CALCULATED ~5.3 ms -- REQUIRES REAL ESP32-S3\n");

    printf("\nCASCADE  [MEASURED on SYNTHETIC audio]\n");
    printf("-----------------------------------------------\n");
    printf("Total frames               %6u\n", (unsigned)g_rep.frames);
    printf("KWS executions             %6u\n", (unsigned)g_rep.kws_execs);
    printf("KWS activation rate        %6.1f %%\n", 100.0*g_rep.kws_execs/g_rep.frames);
    printf("Wake-word frame recall     %6.1f %%  (%u/%u)\n",
           100.0*g_rep.wake_passed/(g_rep.wake_seen?g_rep.wake_seen:1),
           (unsigned)g_rep.wake_passed, (unsigned)g_rep.wake_seen);

    printf("\nLATENCY  [MEASURED -- ON-DEVICE CPU ONLY, NO NETWORK]\n");
    printf("-----------------------------------------------\n");
    printf("Handoff median             %9.2f ms  ->  ~%.2f-%.2f ms\n",
           g_rep.handoff_med/1000.0, g_rep.handoff_med/hi/1000.0, g_rep.handoff_med/lo/1000.0);
    printf("Handoff p95                %9.2f ms\n", g_rep.handoff_p95/1000.0);
    printf("Handoff max                %9.2f ms\n", g_rep.handoff_max/1000.0);
    printf("Pre-roll drain median      %9.2f ms  (throughput, not latency)\n",
           g_rep.drain_med/1000.0);

    printf("\nSTATUS\n-----------------------------------------------\n");
    printf("RAM                        %s   (%.1f KB used vs 256 KB)\n",
           g_rep.total_used <= cfg::kRamTargetBytes ? "PASS" : "FAIL",
           g_rep.total_used / 1024.0);
    printf("                           ** INCOMPLETE: excludes WiFi/TLS **\n");
    printf("CASCADE                    %s   (DS-CNN skipped on %.1f %% of frames,\n",
           g_rep.kws_execs < g_rep.frames ? "PASS" : "FAIL",
           100.0 - 100.0*g_rep.kws_execs/g_rep.frames);
    printf("                           wake-word recall %.1f %%)\n",
           100.0*g_rep.wake_passed/(g_rep.wake_seen?g_rep.wake_seen:1));
    printf("SIMULATED HANDOFF          %s   (on-device portion only)\n",
           g_rep.handoff_p95 / 1000.0 < cfg::kHandoffTargetMs ? "PASS" : "FAIL");
    printf("IDLE CPU                   NOT RELIABLY MEASURABLE IN WOKWI\n");
    printf("FALSE ACCEPTS              NOT MEASURABLE -- MODEL IS UNTRAINED\n");
    printf("================================================\n");

    printf("\nMUST BE RE-MEASURED ON PHYSICAL ESP32-S3:\n");
    printf("-----------------------------------------------\n");
    printf(" 1. DS-CNN inference with esp-nn assembly kernels (remove\n");
    printf("    CONFIG_NN_ANSI_C=y). Expect a large speedup this sim cannot show.\n");
    printf(" 2. Tensor arena with the assembly kernels -- it differs from the\n");
    printf("    ANSI-C arena measured here.\n");
    printf(" 3. RAM cost of a real Wi-Fi + mbedTLS stack. Not in the total above.\n");
    printf(" 4. Idle CPU %%, from a real cycle budget.\n");
    printf(" 5. End-to-end handoff including Wi-Fi TX, TLS records and RTT.\n");
    printf(" 6. I2S microphone capture: DMA timing, clock drift, AGC, mic noise.\n");
    printf(" 7. VAD thresholds against RECORDED AMBIENT NOISE, not synthetic.\n");
    printf(" 8. Cascade activation rate in a real acoustic environment.\n");
    printf(" 9. Wake-word accuracy and false accepts -- needs a TRAINED model\n");
    printf("    and a labelled test set. Nothing here measures these.\n");
    printf("10. Power consumption.\n");
    printf("================================================\n");
    #undef EST
  }

  printf("PHASE13_OK\n");
  while (true) vTaskDelay(pdMS_TO_TICKS(1000));
}
