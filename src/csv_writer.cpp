#include "csv_writer.hpp"
#include <chrono>
#include <cstdio>

namespace blob {

static bool needs_quote(const char* s, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    char c = s[i];
    if (c == ',' || c == '"' || c == '\n' || c == '\r') return true;
  }
  return false;
}

void csv_escape_append(std::string& out, const char* field, size_t n) {
  if (!needs_quote(field, n)) { out.append(field, n); return; }
  out.push_back('"');
  for (size_t i = 0; i < n; ++i) { if (field[i] == '"') out.push_back('"'); out.push_back(field[i]); }
  out.push_back('"');
}

std::string csv_escape(const std::string& field) {
  std::string out;
  csv_escape_append(out, field.data(), field.size());
  return out;
}

// ---- CsvSink ---------------------------------------------------------------
bool CsvSink::open(const std::string& path, bool async, size_t ring_pow2) {
  f_ = std::fopen(path.c_str(), "wb");
  if (!f_) return false;
  async_ = async;
  if (async_) {
    ring_ = std::make_unique<SpscByteRing>(ring_pow2);
    th_ = std::thread(&CsvSink::run, this);
  }
  return true;
}

void CsvSink::write(const char* p, size_t n) {
  if (!f_) return;
  if (!async_) {
    if (std::fwrite(p, 1, n, f_) != n && !werr_.load()) { werr_.store(true); std::fprintf(stderr, "[csv] write error (disk full?)\n"); }
    return;
  }
  // Producer back-pressure: spin/yield until a slot frees (rare).
  int spins = 0;
  while (!ring_->try_write(p, n)) {
    if (++spins < 1024) {
#if defined(__x86_64__) || defined(_M_X64)
      __builtin_ia32_pause();
#endif
    } else {
      std::this_thread::sleep_for(std::chrono::microseconds(50));
      spins = 0;
    }
  }
}

void CsvSink::run() {
  for (;;) {
    size_t n = ring_->drain([this](const char* p, size_t len) {
      if (std::fwrite(p, 1, len, f_) != len && !werr_.load()) {
        werr_.store(true); std::fprintf(stderr, "[csv] write error (disk full?)\n");
      }
    });
    if (flush_req_.load(std::memory_order_acquire) && ring_->empty()) {
      std::fflush(f_);
      flush_req_.store(false, std::memory_order_release);
    }
    if (n == 0) {
      if (stop_.load(std::memory_order_acquire) && ring_->empty()) break;
      std::this_thread::sleep_for(std::chrono::microseconds(100));  // idle backoff
    }
  }
}

void CsvSink::flush() {
  if (!f_) return;
  if (!async_) { std::fflush(f_); return; }
  flush_req_.store(true, std::memory_order_release);
  while (flush_req_.load(std::memory_order_acquire))
    std::this_thread::sleep_for(std::chrono::microseconds(100));
}

void CsvSink::close() {
  if (!f_) return;
  if (async_) {
    stop_.store(true, std::memory_order_release);
    if (th_.joinable()) th_.join();
  }
  std::fflush(f_);
  std::fclose(f_);
  f_ = nullptr;
}

CsvSink::~CsvSink() { close(); }

// ---- MarketDataWriter ------------------------------------------------------
const char* MarketDataWriter::header() {
  return "recv_tsec,recv_tnsec,venue,stream_kind,shard_id,conn_epoch,conn_seq,symbol,payload_json";
}

bool MarketDataWriter::open(const std::string& path, bool async) {
  if (!sink_.open(path, async)) return false;
  fmt_.reserve(4096);
  std::string h = std::string(header()) + "\n";
  sink_.write(h.data(), h.size());
  return true;
}

void MarketDataWriter::write_row(const Timestamp& recv, Venue venue, StreamKind kind,
                                 int shard_id, uint64_t conn_epoch, uint64_t conn_seq,
                                 const std::string& symbol, const std::string& payload_json) {
  char pre[128];
  int n = std::snprintf(pre, sizeof pre, "%lld,%d,%s,%s,%d,%llu,%llu,",
                        (long long)recv.tsec, (int)recv.tnsec, to_string(venue), to_string(kind),
                        shard_id, (unsigned long long)conn_epoch, (unsigned long long)conn_seq);
  fmt_.clear();                              // retains capacity (no alloc)
  fmt_.append(pre, (size_t)n);
  fmt_.append(symbol);
  fmt_.push_back(',');
  csv_escape_append(fmt_, payload_json.data(), payload_json.size());
  fmt_.push_back('\n');
  sink_.write(fmt_.data(), fmt_.size());
}

void MarketDataWriter::flush() { sink_.flush(); }

// ---- OrderBookWriter -------------------------------------------------------
const char* OrderBookWriter::header() {
  return "tsec,tnsec,seqNo,id,type,side,"
         "bid0,bid1,bid2,bid3,bid4,bid_size0,bid_size1,bid_size2,bid_size3,bid_size4,"
         "ask0,ask1,ask2,ask3,ask4,ask_size0,ask_size1,ask_size2,ask_size3,ask_size4";
}

bool OrderBookWriter::open(const std::string& path, bool async) {
  if (!sink_.open(path, async)) return false;
  std::string h = std::string(header()) + "\n";
  sink_.write(h.data(), h.size());
  return true;
}

void OrderBookWriter::write_row(const ObRow& r) {
  char buf[640];
  int n = std::snprintf(buf, sizeof buf,
    "%lld,%d,%llu,%d,%c,%c,"
    "%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,"
    "%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld\n",
    (long long)r.ts.tsec, (int)r.ts.tnsec, (unsigned long long)r.seqNo, (int)r.id, r.type, r.side,
    (long long)r.bid[0],(long long)r.bid[1],(long long)r.bid[2],(long long)r.bid[3],(long long)r.bid[4],
    (long long)r.bid_size[0],(long long)r.bid_size[1],(long long)r.bid_size[2],(long long)r.bid_size[3],(long long)r.bid_size[4],
    (long long)r.ask[0],(long long)r.ask[1],(long long)r.ask[2],(long long)r.ask[3],(long long)r.ask[4],
    (long long)r.ask_size[0],(long long)r.ask_size[1],(long long)r.ask_size[2],(long long)r.ask_size[3],(long long)r.ask_size[4]);
  if (n > 0) sink_.write(buf, (size_t)n);
}

void OrderBookWriter::flush() { sink_.flush(); }

}  // namespace blob
