#pragma once
#include <stdint.h>
#include <stddef.h>

// Every number this module prints comes from an ESP-IDF runtime API or
// esp_timer_get_time(). Nothing here is a constant typed in by hand.

namespace metrics {

// ---- memory ----------------------------------------------------------------
// Internal SRAM only (MALLOC_CAP_INTERNAL). PSRAM is deliberately not built in,
// so these figures are the ones the <=256 KB target is about.
size_t free_internal();
size_t largest_free_block();

// Record a named checkpoint. Delta vs the previous checkpoint is the cost of
// whatever happened in between.
void checkpoint(const char *label);
void memory_report();

// ---- CPU calibration ------------------------------------------------------
// Wokwi is an emulator, not a cycle-accurate model. Before any us/frame figure
// is quoted, this measures a loop of KNOWN cost and compares esp_timer against
// the Xtensa cycle counter, so the distortion factor is visible rather than
// silently baked into every later number.
struct CpuCal {
  uint32_t cycles_per_iter;   // measured
  double   apparent_mhz;      // CCOUNT delta / esp_timer delta
  double   inflation_lo;      // conservative (slower real HW assumed)
  double   inflation_hi;      // optimistic
};
CpuCal calibrate_cpu();

// Print "X us measured -> ~A..B us estimated" for a Wokwi-measured duration.
// Always a RANGE: the deflation reference is an instruction-count argument,
// not a hardware measurement, so a single number would be false precision.
void print_estimate(const char *label, double measured_us, const CpuCal &c);

// ---- timing ----------------------------------------------------------------
// us-resolution. In Wokwi this is simulated time, not silicon time.
struct Stat {
  uint32_t n = 0;
  uint64_t sum_us = 0;
  uint32_t min_us = UINT32_MAX;
  uint32_t max_us = 0;
  void add(uint32_t us);
  double mean_us() const { return n ? (double)sum_us / n : 0.0; }
};

}  // namespace metrics
