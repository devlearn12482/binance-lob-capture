#pragma once
#include <atomic>
#include <cstdint>
#include <ostream>

namespace blob {

// Lightweight observability counters for messages processed, parse errors,
// reconnects, rows written, and book-health checks. Relaxed atomics:
// monotonic counters.
struct Metrics {
  alignas(64) std::atomic<uint64_t> msgs_total{0};
  alignas(64) std::atomic<uint64_t> msgs_depth_diff{0};
  alignas(64) std::atomic<uint64_t> msgs_depth5{0};
  alignas(64) std::atomic<uint64_t> msgs_trade{0};
  alignas(64) std::atomic<uint64_t> parse_errors{0};
  alignas(64) std::atomic<uint64_t> gaps_detected{0};
  alignas(64) std::atomic<uint64_t> reconnects{0};
  alignas(64) std::atomic<uint64_t> resyncs{0};
  alignas(64) std::atomic<uint64_t> depth5_mismatch{0};
  alignas(64) std::atomic<uint64_t> buffer_overflows{0};
  alignas(64) std::atomic<uint64_t> md_rows_written{0};
  alignas(64) std::atomic<uint64_t> ob_rows_written{0};

  void dump(std::ostream& os) const {
    os << "msgs_total="        << msgs_total.load()
       << " depth_diff="       << msgs_depth_diff.load()
       << " depth5="           << msgs_depth5.load()
       << " trade="            << msgs_trade.load()
       << " parse_errors="     << parse_errors.load()
       << " gaps="             << gaps_detected.load()
       << " reconnects="       << reconnects.load()
       << " resyncs="         << resyncs.load()
       << " depth5_mismatch=" << depth5_mismatch.load()
       << " buffer_overflows=" << buffer_overflows.load()
       << " md_rows="          << md_rows_written.load()
       << " ob_rows="          << ob_rows_written.load() << "\n";
  }
};

}  // namespace blob
