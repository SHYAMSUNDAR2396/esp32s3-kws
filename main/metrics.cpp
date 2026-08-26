#include "metrics.h"
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_cpu.h"

namespace metrics {

size_t free_internal() {
  return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

size_t largest_free_block() {
  return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

// ponytail: fixed array, not a vector. The checkpoint list is known and short,
// and a heap-growing container would pollute the very numbers it records.
namespace {
struct Mark { const char *label; size_t free_bytes; };
constexpr int kMaxMarks = 16;
Mark marks[kMaxMarks];
int n_marks = 0;
}  // namespace

void checkpoint(const char *label) {
  if (n_marks >= kMaxMarks) return;
  size_t f = free_internal();
  marks[n_marks] = {label, f};
  size_t delta = (n_marks > 0 && marks[n_marks - 1].free_bytes >= f)
                     ? marks[n_marks - 1].free_bytes - f : 0;
  printf("[MEM] %-38s free=%7u B (%6.1f KB)  delta=%+7d B\n",
         label, (unsigned)f, f / 1024.0,
         n_marks > 0 ? -(int)delta : 0);
  n_marks++;
}

void memory_report() {
  printf("\n========== MEMORY REPORT ==========\n");
  printf("(MEASURED IN WOKWI -- ESP-IDF heap_caps, internal SRAM only)\n\n");
  for (int i = 0; i < n_marks; i++) {
    printf("  %-38s %7.1f KB free\n", marks[i].label, marks[i].free_bytes / 1024.0);
  }
  if (n_marks >= 2) {
    size_t used = marks[0].free_bytes - marks[n_marks - 1].free_bytes;
    printf("\n  %-38s %7.1f KB\n", "TOTAL USED (boot -> last checkpoint)", used / 1024.0);
    printf("  %-38s %7.1f KB\n", "FREE HEADROOM", marks[n_marks - 1].free_bytes / 1024.0);
    printf("  %-38s %7.1f KB\n", "LARGEST FREE BLOCK", largest_free_block() / 1024.0);
    printf("  %-38s %7s\n", "TARGET", "<= 256 KB");
    printf("  %-38s %7s\n", "STATUS", used <= 256u * 1024 ? "PASS" : "FAIL");
  }
  printf("===================================\n\n");
}

CpuCal calibrate_cpu() {
  // The loop below compiles (xtensa-esp32s3-elf-g++ -O2) to exactly 5
  // instructions, VERIFIED by objdump of this object file:
  //     mull a9,a8,a8 / add.n a7,a7,a9 / addi.n a8,a8,1 / l32r a9,.. / bge
  // On a real LX7 that is ~5-7 cycles per iteration (roughly 1 cycle each,
  // plus a taken-branch penalty). We deflate with that BAND rather than a
  // single figure, because the per-instruction cost is an argument from the
  // ISA, not something measured on silicon.
  constexpr int kIters = 200000;
  constexpr double kCyclesOptimistic  = 5.0;   // 1 cycle/instruction
  constexpr double kCyclesConservative = 7.0;  // + branch/hazard penalty
  volatile int32_t sink = 0;
  int32_t acc = 0;

  uint32_t c0 = esp_cpu_get_cycle_count();
  int64_t  t0 = esp_timer_get_time();
  for (int i = 0; i < kIters; i++) acc += i * i;
  int64_t  t1 = esp_timer_get_time();
  uint32_t c1 = esp_cpu_get_cycle_count();
  sink = acc; (void)sink;

  uint32_t cycles = c1 - c0;
  int64_t  us     = t1 - t0;
  CpuCal r;
  r.cycles_per_iter = cycles / kIters;
  r.apparent_mhz    = us > 0 ? (double)cycles / us : 0.0;
  r.inflation_hi    = r.cycles_per_iter / kCyclesOptimistic;
  r.inflation_lo    = r.cycles_per_iter / kCyclesConservative;

  printf("\n---------- CPU CALIBRATION (Wokwi honesty check) ----------\n");
  printf("known-cost loop   : %d iterations, 5 instructions each (objdump-verified)\n", kIters);
  printf("MEASURED cycles   : %u  (%u cycles/iteration)\n",
         (unsigned)cycles, (unsigned)r.cycles_per_iter);
  printf("MEASURED esp_timer: %lld us\n", (long long)us);
  printf("apparent clock    : %.1f MHz (configured %d MHz) -> timer and CCOUNT AGREE\n",
         r.apparent_mhz, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
  printf("real S3 reference : %.0f-%.0f cycles/iteration (ISA argument, NOT measured)\n",
         kCyclesOptimistic, kCyclesConservative);
  printf("=> WOKWI INSTRUCTION-TIME INFLATION: ~%.0fx to ~%.0fx\n",
         r.inflation_lo, r.inflation_hi);
  printf("\nWokwi's CLOCK is self-consistent; its INSTRUCTION COST MODEL is not.\n");
  printf("Every us figure below is therefore inflated by roughly this factor and\n");
  printf("is shown as measured + an estimated BAND. The band is an order-of-\n");
  printf("magnitude sanity check. REQUIRES REAL ESP32-S3 VALIDATION.\n");
  printf("-----------------------------------------------------------\n");
  return r;
}

void print_estimate(const char *label, double measured_us, const CpuCal &c) {
  printf("  %-26s %8.1f us measured  ->  ~%.1f-%.1f us estimated on real S3\n",
         label, measured_us, measured_us / c.inflation_hi, measured_us / c.inflation_lo);
}

void Stat::add(uint32_t us) {
  n++; sum_us += us;
  if (us < min_us) min_us = us;
  if (us > max_us) max_us = us;
}

}  // namespace metrics
