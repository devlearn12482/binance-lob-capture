#include "config.hpp"
#include "csv_writer.hpp"
#include "json_min.hpp"
#include "metrics.hpp"
#include "book_session.hpp"
#include "order_book.hpp"
#include "replay.hpp"
#include "snapshot.hpp"
#include "ws_client.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace blob;

// Async-signal-safe: the handler only sets an atomic flag. A normal watcher
// thread observes it and performs the (non-signal-safe) stop() calls.
static std::atomic<bool> g_stop{false};
static void on_signal(int) { g_stop.store(true); }

static Timestamp now_ts() {
  auto n = std::chrono::system_clock::now().time_since_epoch();
  auto s = std::chrono::duration_cast<std::chrono::seconds>(n);
  auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(n) -
            std::chrono::duration_cast<std::chrono::nanoseconds>(s);
  Timestamp t; t.tsec = s.count(); t.tnsec = (int32_t)ns.count(); return t;
}

static Timestamp ms_to_ts(int64_t ms) {
  Timestamp t; t.tsec = ms / 1000; t.tnsec = (int32_t)((ms % 1000) * 1000000); return t;
}

static std::string utc_date() {
  std::time_t t = std::time(nullptr); std::tm tm{}; gmtime_r(&t, &tm);
  char buf[16]; std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm); return buf;
}

static int32_t derive_id(const std::string& s) {
  uint32_t h = 2166136261u; for (char c : s) { h ^= (uint8_t)c; h *= 16777619u; }
  return (int32_t)(h & 0x7fffffff);
}

// Per-symbol live state (accessed only by its owning shard thread).
struct SymState {
  std::unique_ptr<BookSession> sess;
  std::unique_ptr<OrderBookWriter> ob_writer;
  DepthEvent scratch;
  uint64_t ob_seq = 0;
  uint64_t last_epoch = UINT64_MAX;  // detect reconnects (new conn_epoch)
};

// One shard = one WebSocket connection + its own writers/books. Because each
// shard owns its writers and each symbol is owned by exactly one shard, every
// CsvSink has a single producer thread (true SPSC) and there is no shared
// mutable state except the atomic Metrics counters.
static int run_shard(const Config& cfg, int shard_id,
                      const std::vector<std::string>& syms,
                      IWsClient* client, Metrics& metrics) {
  const std::string date = utc_date();
  const std::string venue = to_string(cfg.venue);
  // File tag. Single shard keeps the documented naming (symbol, or "multi" with
  // a symbol column). With >1 shard, every file is prefixed with its shard index
  // for consistency: s0_multi, s1_ETHUSDT, ...
  std::string base = (syms.size() == 1) ? syms[0] : std::string("multi");
  std::string tag = (cfg.shards > 1) ? ("s" + std::to_string(shard_id) + "_" + base) : base;

  MarketDataWriter md;
  std::string md_path = cfg.output_dir + "/market_data_" + venue + "_" + tag + "_" + date + ".csv";
  if (!md.open(md_path, !cfg.sync_writes)) { std::fprintf(stderr, "cannot open %s\n", md_path.c_str()); return 2; }

  CsvSink snaps;  // snapshot sidecar (small ring; snapshots are rare/large)
  std::string snap_path = md_path.substr(0, md_path.size() - 4) + "_snapshots.csv";
  if (!snaps.open(snap_path, !cfg.sync_writes, 1u << 6)) { std::fprintf(stderr, "cannot open %s\n", snap_path.c_str()); return 2; }
  snaps.write("recv_tsec,recv_tnsec,symbol,lastUpdateId,payload_json\n");

  std::map<std::string, SymState> sym;
  for (const auto& s : syms) {
    SymState st;
    st.sess = std::make_unique<BookSession>(derive_id(s), cfg.venue);
    st.ob_writer = std::make_unique<OrderBookWriter>();
    st.scratch.bids.reserve(64);
    st.scratch.asks.reserve(64);
    std::string p = cfg.output_dir + "/market_data_" + venue + "_" + s + "_" + date + "_orderbook.csv";
    if (!st.ob_writer->open(p, !cfg.sync_writes)) { std::fprintf(stderr, "cannot open %s\n", p.c_str()); return 2; }
    sym.emplace(s, std::move(st));
  }

  auto resync = [&](SymState& st, const std::string& symbol) -> bool {
    DepthEvent snap; std::string serr, raw;
    int limit = (cfg.venue == Venue::Spot) ? 5000 : 1000;
    if (!fetch_depth_snapshot(cfg.venue, symbol, limit, snap, serr, &raw)) {
      std::fprintf(stderr, "[resync] %s snapshot failed: %s\n", symbol.c_str(), serr.c_str());
      return false;
    }
    st.sess->on_snapshot(snap.bids, snap.asks, snap.lastUpdateId);
    metrics.resyncs++;
    Timestamp t = now_ts();
    char pre[96];
    int n = std::snprintf(pre, sizeof pre, "%lld,%d,", (long long)t.tsec, (int)t.tnsec);
    std::string row(pre, (size_t)n);
    row += symbol; row += ',';
    row += std::to_string((unsigned long long)snap.lastUpdateId); row += ',';
    csv_escape_append(row, raw.data(), raw.size()); row += '\n';
    snaps.write(row.data(), row.size());
    return st.sess->book().initialized() && !st.sess->resync_pending();
  };

  std::string stream;
  std::string data;
  std::string sym_str;  // reused per message — assign() retains capacity, no alloc
  stream.reserve(64);
  data.reserve(8192);
  sym_str.reserve(16);

  auto handler = [&](const std::string& frame, uint64_t conn_epoch, uint64_t conn_seq) {
    stream.clear();
    data.clear();
    if (!split_envelope(frame, stream, data)) { metrics.parse_errors++; return; }
    // Extract symbol (part before first '@') and uppercase in-place.
    // Uses assign() on a hoisted string — zero heap allocation in steady state.
    size_t at = stream.find('@');
    sym_str.assign(stream, 0, at == std::string::npos ? stream.size() : at);
    for (auto& c : sym_str) c = (char)::toupper(c);

    StreamKind kind = classify_stream(stream);   // by stream name (authoritative)
    auto it = sym.find(sym_str);
    SymState* stp = (it == sym.end()) ? nullptr : &it->second;
    DepthEvent* parsed_depth = nullptr;
    bool parse_failed = false;

    Timestamp recv_ts = now_ts();
    Timestamp ts = recv_ts;
    if ((kind == StreamKind::DepthDiff || kind == StreamKind::Depth5) && stp != nullptr) {
      parsed_depth = &stp->scratch;
      reset_depth_event(*parsed_depth);
      if (!parse_depth(data, *parsed_depth)) {
        metrics.parse_errors++;
        parse_failed = true;
      } else if (cfg.time_policy == TimePolicy::BinanceEventTime && parsed_depth->event_time_ms > 0) {
        ts = ms_to_ts(parsed_depth->event_time_ms);
      }
    } else if (cfg.time_policy == TimePolicy::BinanceEventTime) {
      int64_t et = event_time_ms(data);  // Trades are not depth-parsed.
      if (et > 0) ts = ms_to_ts(et);
    }

    md.write_row(ts, cfg.venue, kind, shard_id, conn_epoch, conn_seq, sym_str, data);
    metrics.md_rows_written++; metrics.msgs_total++;
    if (parse_failed || stp == nullptr) return;

    SymState& st = *stp;

    // Reconnect (new conn_epoch): invalidate the book and force a fresh resync
    // before processing this connection's first event for the symbol.
    if (st.last_epoch != conn_epoch) {
      if (st.last_epoch != UINT64_MAX) st.sess->request_resync();
      st.last_epoch = conn_epoch;
    }

    char type = '?'; bool emit = false;
    if (kind == StreamKind::DepthDiff) {
      metrics.msgs_depth_diff++;
      DepthEvent& ev = *parsed_depth;
      auto o = st.sess->on_diff(ev);
      if (o == BookSession::Outcome::Applied) { emit = true; type = 'D'; }
      else if (o == BookSession::Outcome::ResyncNeeded ||
               (o == BookSession::Outcome::Buffered && st.sess->resync_pending())) {
        if (o == BookSession::Outcome::ResyncNeeded) metrics.gaps_detected++;
        if (resync(st, sym_str)) { emit = true; type = 'D'; }
      }
    } else if (kind == StreamKind::Depth5) {
      metrics.msgs_depth5++;
      DepthEvent& ev = *parsed_depth;
      // depth5 is the exchange's authoritative top-5 partial snapshot. It uses
      // REPLACE semantics: emit a 'type=5' row built directly from the snapshot
      // (no stale levels even when the price band moves). Also cross-check it
      // against the diff-maintained book for observability.
      if (st.sess->initialized()) {
        ObRow chk; st.sess->book().fill_top5(chk);
        if (!ev.bids.empty() && chk.bid[0] != ev.bids[0].price) metrics.depth5_mismatch++;
      }
      // NOTE: we do NOT feed depth5 into the diff book — the diff stream is the
      // sole, complete source of book state. depth5 is emitted as its own row.
      ObRow row; row.ts = ts; row.seqNo = ++st.ob_seq; row.id = st.sess->book().id();
      row.type = '5'; row.side = 'N';
      for (size_t i = 0; i < ev.bids.size() && i < 5; ++i) { row.bid[i] = ev.bids[i].price; row.bid_size[i] = ev.bids[i].qty; }
      for (size_t i = 0; i < ev.asks.size() && i < 5; ++i) { row.ask[i] = ev.asks[i].price; row.ask_size[i] = ev.asks[i].qty; }
      st.ob_writer->write_row(row); metrics.ob_rows_written++;
      return;  // depth5 fully handled (self-contained snapshot)
    } else if (kind == StreamKind::Trade) {
      metrics.msgs_trade++; if (cfg.integrate_trades) { emit = true; type = 'T'; }
    }

    // Fail closed: never emit a row from an unseeded or resync-pending book.
    if (emit && (!st.sess->initialized() || st.sess->resync_pending())) emit = false;
    if (emit) {
      ObRow row; row.ts = ts; row.seqNo = ++st.ob_seq; row.type = type; row.side = 'N';
      st.sess->book().fill_top5(row);
      st.ob_writer->write_row(row); metrics.ob_rows_written++;
    }
  };

  Config scfg = cfg;
  scfg.symbols = syms;
  scfg.shard_id = shard_id;
  int rc = client->run(scfg, metrics, handler);   // stub returns nonzero

  md.flush();
  snaps.flush();
  bool wrote_ok = !md.failed() && !snaps.failed();
  for (auto& kv : sym) {
    kv.second.ob_writer->flush();
    if (kv.second.ob_writer->failed()) wrote_ok = false;
    metrics.buffer_overflows.fetch_add(kv.second.sess->buffer_overflows());
  }
  if (rc == 0 && !wrote_ok) rc = 4;   // a write failed (e.g. disk full)
  return rc;
}

int main(int argc, char** argv) {
  std::string err;
  Config cfg = Config::from_args(argc, argv, err);
  if (!err.empty()) {
    if (err == "help") { std::cout << Config::usage(); return 0; }
    std::cerr << "error: " << err << "\n\n" << Config::usage(); return 1;
  }

  Metrics metrics;

  if (!cfg.replay_input.empty()) {
    int rc = run_replay(cfg, metrics);
    metrics.dump(std::cerr);
    return rc;
  }

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  auto groups = shard_symbols(cfg.symbols, cfg.shards);
  std::vector<std::unique_ptr<IWsClient>> clients;
  std::vector<std::thread> threads;
  std::vector<int> rcs(groups.size(), 0);   // per-shard exit codes
  for (size_t s = 0; s < groups.size(); ++s) {
    if (groups[s].empty()) continue;
    auto client = make_ws_client();
    IWsClient* raw = client.get();
    clients.push_back(std::move(client));
    int sid = (int)s;
    const std::vector<std::string>& syms = groups[s];
    threads.emplace_back([&cfg, sid, syms, raw, &metrics, &rcs]() {
      rcs[(size_t)sid] = run_shard(cfg, sid, syms, raw, metrics);
    });
  }

  // Watcher: observe the signal flag and stop all clients from a normal thread.
  std::atomic<bool> workers_done{false};
  std::thread watcher([&]() {
    while (!workers_done.load()) {
      if (g_stop.load()) { for (auto& c : clients) c->stop(); break; }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  });

  for (auto& t : threads) if (t.joinable()) t.join();
  workers_done.store(true);
  watcher.join();

  metrics.dump(std::cerr);
  int rc = 0;
  for (int r : rcs) if (r != 0) rc = r;   // surface any shard failure
  return rc;
}
