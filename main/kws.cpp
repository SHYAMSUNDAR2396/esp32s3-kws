#include "kws.h"
#include "ds_cnn_model_data.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "esp_heap_caps.h"

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

namespace kws {
namespace {

// Arena is sized generously, then we report what TFLM ACTUALLY used. Guessing
// the arena and declaring the guess would be exactly the kind of made-up
// number this project is trying to avoid.
constexpr size_t kArenaCapacity = 40 * 1024;  // measured need ~31.2 KB + margin
uint8_t *g_arena = nullptr;

const tflite::Model *g_model = nullptr;
tflite::MicroInterpreter *g_interp = nullptr;
TfLiteTensor *g_in = nullptr, *g_out = nullptr;

// Sliding log-mel context window, oldest-first.
float g_ctx[kContextFrames][cfg::kMelBins];
int   g_ctx_fill = 0;    // frames written since reset, saturating
int   g_ctx_head = 0;    // next write slot

DecisionConfig g_dcfg;
float    g_hist[16];
int      g_hist_n = 0, g_hist_head = 0;
int64_t  g_last_fire_us = -1;

using Resolver = tflite::MicroMutableOpResolver<8>;
Resolver g_resolver;

}  // namespace

size_t arena_capacity() { return kArenaCapacity; }
size_t model_bytes()    { return g_ds_cnn_model_len; }
size_t arena_used()     { return g_interp ? g_interp->arena_used_bytes() : 0; }

bool init() {
  g_model = tflite::GetModel(g_ds_cnn_model);
  if (g_model->version() != TFLITE_SCHEMA_VERSION) {
    printf("[KWS] schema mismatch: model %lu, runtime %d\n",
           (unsigned long)g_model->version(), TFLITE_SCHEMA_VERSION);
    return false;
  }

  // Only the ops this graph actually contains -- a full resolver would drag in
  // every kernel and inflate flash for nothing.
  if (g_resolver.AddConv2D()          != kTfLiteOk) return false;
  if (g_resolver.AddDepthwiseConv2D() != kTfLiteOk) return false;
  if (g_resolver.AddFullyConnected()  != kTfLiteOk) return false;
  if (g_resolver.AddSoftmax()         != kTfLiteOk) return false;
  if (g_resolver.AddMean()            != kTfLiteOk) return false;
  if (g_resolver.AddReshape()         != kTfLiteOk) return false;
  if (g_resolver.AddQuantize()        != kTfLiteOk) return false;
  if (g_resolver.AddDequantize()      != kTfLiteOk) return false;

  g_arena = (uint8_t *)heap_caps_malloc(kArenaCapacity,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!g_arena) { printf("[KWS] arena alloc failed\n"); return false; }

  static tflite::MicroInterpreter interp(g_model, g_resolver, g_arena, kArenaCapacity);
  g_interp = &interp;
  if (g_interp->AllocateTensors() != kTfLiteOk) {
    printf("[KWS] AllocateTensors failed\n");
    return false;
  }
  g_in  = g_interp->input(0);
  g_out = g_interp->output(0);
  reset_decision();
  return true;
}

void tensor_info() {
  printf("  input  dims  : [");
  for (int i = 0; i < g_in->dims->size; i++)
    printf("%d%s", g_in->dims->data[i], i + 1 < g_in->dims->size ? ", " : "");
  printf("]  type=%s  scale=%.6f  zero_point=%d\n",
         g_in->type == kTfLiteInt8 ? "int8" : "other",
         g_in->params.scale, (int)g_in->params.zero_point);
  printf("  output dims  : [");
  for (int i = 0; i < g_out->dims->size; i++)
    printf("%d%s", g_out->dims->data[i], i + 1 < g_out->dims->size ? ", " : "");
  printf("]  type=%s  scale=%.6f  zero_point=%d\n",
         g_out->type == kTfLiteInt8 ? "int8" : "other",
         g_out->params.scale, (int)g_out->params.zero_point);
  printf("  input bytes  : %u   output bytes : %u\n",
         (unsigned)g_in->bytes, (unsigned)g_out->bytes);
}

bool push_frame(const float *mel) {
  memcpy(g_ctx[g_ctx_head], mel, sizeof(float) * cfg::kMelBins);
  g_ctx_head = (g_ctx_head + 1) % kContextFrames;
  if (g_ctx_fill < kContextFrames) g_ctx_fill++;
  return g_ctx_fill >= kContextFrames;
}

float infer() {
  if (!g_interp || g_ctx_fill < kContextFrames) return 0.0f;

  // Quantise the context window into the input tensor, oldest frame first.
  int8_t *dst = g_in->data.int8;
  const float scale = g_in->params.scale;
  const int   zp    = g_in->params.zero_point;
  for (int t = 0; t < kContextFrames; t++) {
    const float *src = g_ctx[(g_ctx_head + t) % kContextFrames];
    for (int m = 0; m < cfg::kMelBins; m++) {
      int v = (int)lrintf(src[m] / scale) + zp;
      if (v < -128) v = -128;
      if (v >  127) v =  127;
      *dst++ = (int8_t)v;
    }
  }

  if (g_interp->Invoke() != kTfLiteOk) return 0.0f;

  // Class 1 = wake. Dequantise to a probability.
  int8_t q = g_out->data.int8[1];
  return (q - g_out->params.zero_point) * g_out->params.scale;
}

void configure(const DecisionConfig &c) { g_dcfg = c; reset_decision(); }

void reset_decision() {
  g_hist_n = 0; g_hist_head = 0; g_last_fire_us = -1;
  memset(g_hist, 0, sizeof(g_hist));
}

Decision decide(float raw, int64_t now_us) {
  int w = g_dcfg.smooth_window;
  if (w > (int)(sizeof(g_hist) / sizeof(float))) w = sizeof(g_hist) / sizeof(float);

  g_hist[g_hist_head] = raw;
  g_hist_head = (g_hist_head + 1) % w;
  if (g_hist_n < w) g_hist_n++;

  float sum = 0.0f;
  for (int i = 0; i < g_hist_n; i++) sum += g_hist[i];
  float smoothed = sum / g_hist_n;

  bool cooling = g_last_fire_us >= 0 &&
                 (now_us - g_last_fire_us) < (int64_t)g_dcfg.cooldown_ms * 1000;

  bool fire = false;
  // Require a full smoothing window: firing on a partially-filled average
  // makes the first frames after boot or after a reset trigger-happy.
  if (!cooling && g_hist_n >= w && smoothed > g_dcfg.threshold) {
    fire = true;
    g_last_fire_us = now_us;
  }
  return {raw, smoothed, fire, cooling};
}

}  // namespace kws
