#pragma once
#include <stdint.h>
#include <stddef.h>
#include "config.h"

// Continuously-overwritten pre-roll buffer. Audio is ALWAYS being written; the
// wake-word trigger is a reader, never a starter. That is the whole point --
// by the time KWS is confident, the word itself is already ~300 ms in the past.
class RingBuffer {
 public:
  ~RingBuffer() { free(); }
  bool init();                       // allocates from internal SRAM
  void free();
  void write(const int16_t *src, size_t n);

  // Copy the most recent `n` samples in chronological order. Returns the
  // number actually available (< n only before the buffer has filled once).
  size_t read_last(int16_t *dst, size_t n) const;

  size_t capacity_samples() const { return cfg::kRingSamples; }
  size_t allocated_bytes() const { return allocated_; }
  uint64_t total_written() const { return total_; }
  bool wrapped() const { return total_ >= cfg::kRingSamples; }

 private:
  int16_t *buf_ = nullptr;
  size_t allocated_ = 0;
  size_t w_ = 0;        // write index, wraps
  uint64_t total_ = 0;  // lifetime samples written
};
