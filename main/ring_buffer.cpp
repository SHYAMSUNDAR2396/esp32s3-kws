#include "ring_buffer.h"
#include <string.h>
#include <stdio.h>
#include "esp_heap_caps.h"

bool RingBuffer::init() {
  // Internal SRAM explicitly: PSRAM would make the RAM figure a lie.
  buf_ = (int16_t *)heap_caps_malloc(cfg::kRingBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!buf_) return false;
  allocated_ = heap_caps_get_allocated_size(buf_);  // real size incl. heap rounding
  memset(buf_, 0, cfg::kRingBytes);
  w_ = 0; total_ = 0;
  return true;
}

void RingBuffer::free() {
  if (buf_) { heap_caps_free(buf_); buf_ = nullptr; allocated_ = 0; }
}

void RingBuffer::write(const int16_t *src, size_t n) {
  if (!buf_) return;
  while (n > 0) {
    size_t chunk = cfg::kRingSamples - w_;
    if (chunk > n) chunk = n;
    memcpy(buf_ + w_, src, chunk * sizeof(int16_t));
    w_ = (w_ + chunk) % cfg::kRingSamples;
    src += chunk; n -= chunk; total_ += chunk;
  }
}

size_t RingBuffer::read_last(int16_t *dst, size_t n) const {
  if (!buf_) return 0;
  size_t have = total_ < cfg::kRingSamples ? (size_t)total_ : cfg::kRingSamples;
  if (n > have) n = have;
  // start = n samples back from the write head, modulo capacity
  size_t start = (w_ + cfg::kRingSamples - n) % cfg::kRingSamples;
  size_t first = cfg::kRingSamples - start;
  if (first > n) first = n;
  memcpy(dst, buf_ + start, first * sizeof(int16_t));
  if (n > first) memcpy(dst + first, buf_, (n - first) * sizeof(int16_t));
  return n;
}
