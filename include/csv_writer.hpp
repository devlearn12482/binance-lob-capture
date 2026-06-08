#pragma once
#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include "order_book.hpp"
#include "spsc_ring.hpp"
#include "types.hpp"

namespace blob {

// RFC 4180 field escaping.
std::string csv_escape(const std::string& field);
// Append the escaped form of `field` to `out` (no fresh allocation beyond out's
// own growth) — used to format rows into a reused buffer.
void csv_escape_append(std::string& out, const char* field, size_t n);

// Single-producer / single-consumer line sink backed by a lock-free SPSC ring.
// One thread (the owning shard) pushes records; a background writer thread owns
// the FILE* and drains them. Slot buffers are reused, so steady-state writes do
// no heap allocation. In sync mode it writes inline.
//
// Thread model: write()/flush()/close() are all called by the SINGLE producer
// thread that owns this sink; only the consumer thread touches the FILE*.
class CsvSink {
public:
  CsvSink() = default;
  ~CsvSink();
  CsvSink(const CsvSink&) = delete;
  CsvSink& operator=(const CsvSink&) = delete;

  [[nodiscard]] bool open(const std::string& path, bool async = true, size_t ring_pow2 = 1u << 13);
  void write(const char* p, size_t n);          // hot path (no allocation)
  void write(const std::string& s) { write(s.data(), s.size()); }
  void flush();
  void close();
  [[nodiscard]] bool failed() const { return werr_.load(); }

private:
  void run();

  std::FILE* f_ = nullptr;
  bool async_ = true;
  std::thread th_;
  std::unique_ptr<SpscByteRing> ring_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> flush_req_{false};
  std::atomic<bool> werr_{false};   // sticky write-error flag (logged once)
};

// Deliverable A: market-data audit CSV (one row per inbound message).
class MarketDataWriter {
public:
  [[nodiscard]] bool open(const std::string& path, bool async = true);
  void write_row(const Timestamp& recv, Venue venue, StreamKind kind,
                 int shard_id, uint64_t conn_epoch, uint64_t conn_seq,
                 const std::string& symbol, const std::string& payload_json);
  void flush();
  [[nodiscard]] bool failed() const { return sink_.failed(); }
  static const char* header();
private:
  CsvSink sink_;
  std::string fmt_;   // reused per-row format buffer (single producer)
};

// Deliverable B: order-book snapshot CSV (exact 26-column rows).
class OrderBookWriter {
public:
  [[nodiscard]] bool open(const std::string& path, bool async = true);
  void write_row(const ObRow& row);
  void flush();
  [[nodiscard]] bool failed() const { return sink_.failed(); }
  static const char* header();
private:
  CsvSink sink_;
};

}  // namespace blob
