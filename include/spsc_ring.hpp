#pragma once
#include <atomic>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace blob {

// Lock-free single-producer / single-consumer byte-record ring.
//
// One thread pushes records (try_write), one thread drains them. Each slot owns
// a std::string whose capacity is retained across reuse, so in steady state
// push/drain perform NO heap allocation (the slot buffer is reused via assign).
// head_/tail_ are on separate cache lines to avoid false sharing.
//
// Memory ordering: producer publishes a slot with a release store to tail_;
// consumer observes it with an acquire load. Consumer frees a slot with a
// release store to head_; producer observes free space with an acquire load.
class SpscByteRing {
public:
  explicit SpscByteRing(size_t capacity_pow2)
      : mask_(capacity_pow2 - 1), slots_(capacity_pow2) {
    // capacity must be a power of two
  }

  [[nodiscard]] bool empty() const {
    return head_.load(std::memory_order_acquire) ==
           tail_.load(std::memory_order_acquire);
  }

  // Producer: copy [p,p+n) into the next free slot. Returns false if full.
  [[nodiscard]] bool try_write(const char* p, size_t n) {
    const size_t t = tail_.load(std::memory_order_relaxed);
    const size_t next = (t + 1) & mask_;
    if (next == head_.load(std::memory_order_acquire)) return false;  // full
    slots_[t].assign(p, n);                       // reuses retained capacity
    tail_.store(next, std::memory_order_release);  // publish
    return true;
  }

  // Consumer: invoke fn(const char*, size_t) for every available record.
  // Returns the number of records drained.
  template <class F>
  size_t drain(F&& fn) {
    size_t h = head_.load(std::memory_order_relaxed);
    const size_t t = tail_.load(std::memory_order_acquire);
    size_t count = 0;
    while (h != t) {
      const std::string& rec = slots_[h];
      fn(rec.data(), rec.size());
      h = (h + 1) & mask_;
      ++count;
    }
    if (count) head_.store(h, std::memory_order_release);  // free slots
    return count;
  }

private:
  size_t mask_;
  std::vector<std::string> slots_;
  alignas(64) std::atomic<size_t> head_{0};  // consumer index (read position)
  alignas(64) std::atomic<size_t> tail_{0};  // producer index (write position)
  char pad_[64];                              // isolate from following members
};

}  // namespace blob
