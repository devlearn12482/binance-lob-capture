#include "replay.hpp"
#include "csv_writer.hpp"
#include "book_session.hpp"
#include "order_book.hpp"
#include "json_min.hpp"
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace blob {

// Parse one RFC-4180 CSV line into fields (handles quoted fields and "" escapes).
static std::vector<std::string> parse_csv_line(const std::string& line) {
  std::vector<std::string> f; std::string cur; bool q = false;
  for (size_t i = 0; i < line.size(); ++i) {
    char ch = line[i];
    if (q) {
      if (ch == '"') { if (i + 1 < line.size() && line[i+1] == '"') { cur.push_back('"'); ++i; } else q = false; }
      else cur.push_back(ch);
    } else {
      if (ch == '"') q = true;
      else if (ch == ',') { f.push_back(cur); cur.clear(); }
      else cur.push_back(ch);
    }
  }
  f.push_back(cur);
  return f;
}

static int32_t derive_id(const std::string& symbol) {
  uint32_t h = 2166136261u;
  for (char c : symbol) { h ^= (uint8_t)c; h *= 16777619u; }
  return (int32_t)(h & 0x7fffffff);
}

int run_replay(const Config& cfg, Metrics& m) {
  std::ifstream in(cfg.replay_input);
  if (!in) { std::fprintf(stderr, "replay: cannot open %s\n", cfg.replay_input.c_str()); return 2; }

  // Load REST snapshots recorded by the live run (if present) so replay can
  // reproduce the exact REST-anchored book rather than bootstrapping from diffs.
  // Sidecar path: <name>.csv -> <name>_snapshots.csv
  std::map<std::string, std::deque<DepthEvent>> snaps;
  bool have_sidecar = false;
  {
    std::string sc = cfg.replay_input;
    if (sc.size() > 4 && sc.compare(sc.size()-4, 4, ".csv") == 0) sc = sc.substr(0, sc.size()-4);
    sc += "_snapshots.csv";
    std::ifstream sin(sc);
    if (sin) {
      have_sidecar = true;
      std::string sl; std::getline(sin, sl);  // header
      // Columns: recv_tsec,recv_tnsec,symbol,lastUpdateId,payload_json
      while (std::getline(sin, sl)) {
        if (sl.empty()) continue;
        auto f = parse_csv_line(sl);
        if (f.size() < 5) continue;
        DepthEvent ev;
        if (parse_depth(f[4], ev)) snaps[f[2]].push_back(ev);
        else m.parse_errors++;
      }
    }
  }

  std::map<std::string, BookSession>     books;
  std::map<std::string, std::unique_ptr<OrderBookWriter>> writers;
  std::map<std::string, uint64_t>        seqs;
  std::map<std::string, uint64_t>        epochs;  // last conn_epoch per symbol
  std::map<std::string, DepthEvent>      scratch;

  // Seed a session from the next recorded REST snapshot; false if none left.
  auto seed_from_sidecar = [&](const std::string& symbol, BookSession& ob) -> bool {
    auto it = snaps.find(symbol);
    if (it == snaps.end() || it->second.empty()) return false;
    DepthEvent sn = it->second.front();
    it->second.pop_front();
    ob.on_snapshot(sn.bids, sn.asks, sn.lastUpdateId);
    m.resyncs++;
    return ob.initialized() && !ob.resync_pending();
  };

  std::string line;
  std::getline(in, line);  // header
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    auto f = parse_csv_line(line);
    if (f.size() < 9) { m.parse_errors++; continue; }
    Timestamp ts; ts.tsec = std::atoll(f[0].c_str()); ts.tnsec = std::atoi(f[1].c_str());
    const std::string& kind = f[3];
    const std::string& symbol = f[7];
    const std::string& payload = f[8];
    m.msgs_total++;

    if (books.find(symbol) == books.end()) {
      auto w = std::make_unique<OrderBookWriter>();
      std::string path = cfg.output_dir + "/replay_" + symbol + "_orderbook.csv";
      if (!w->open(path)) { std::fprintf(stderr, "replay: cannot open %s\n", path.c_str()); return 2; }
      books.emplace(symbol, BookSession(derive_id(symbol), cfg.venue));
      writers.emplace(symbol, std::move(w));
      seqs[symbol] = 0;
      DepthEvent ev;
      ev.bids.reserve(64);
      ev.asks.reserve(64);
      scratch.emplace(symbol, std::move(ev));
    }
    BookSession& ob = books.at(symbol);

    // Mirror live: a new conn_epoch (a reconnect recorded in the capture)
    // invalidates the book, so the matching snapshot sidecar entry is consumed
    // at the same point and live output is reproduced.
    uint64_t ce = std::strtoull(f[5].c_str(), nullptr, 10);
    auto ei = epochs.find(symbol);
    if (ei == epochs.end()) epochs[symbol] = ce;
    else if (ei->second != ce) { ob.request_resync(); ei->second = ce; }

    char type = '?'; bool emit = false;
    if (kind == "depth_diff") {
      m.msgs_depth_diff++;
      DepthEvent& ev = scratch.at(symbol);
      reset_depth_event(ev);
      if (!parse_depth(payload, ev)) { m.parse_errors++; continue; }
      auto o = ob.on_diff(ev);
      if (o == BookSession::Outcome::Applied) { emit = true; type = 'D'; }
      else if (o == BookSession::Outcome::ResyncNeeded ||
               (o == BookSession::Outcome::Buffered && ob.resync_pending())) {
        if (o == BookSession::Outcome::ResyncNeeded) m.gaps_detected++;
        // Prefer a recorded REST snapshot (faithful to live). Without the
        // sidecar, keep buffering until the next depth5 checkpoint in the
        // market-data CSV seeds the replay book.
        if (seed_from_sidecar(symbol, ob)) { emit = true; type = 'D'; }
      }
    } else if (kind == "depth5") {
      m.msgs_depth5++;
      DepthEvent& ev = scratch.at(symbol);
      reset_depth_event(ev);
      if (!parse_depth(payload, ev)) { m.parse_errors++; continue; }
      if (ob.initialized()) {
        ObRow chk; ob.book().fill_top5(chk);
        if (!ev.bids.empty() && chk.bid[0] != ev.bids[0].price) m.depth5_mismatch++;
      }
      if (!have_sidecar) {
        ob.on_snapshot(ev.bids, ev.asks, ev.lastUpdateId);
      }
      // depth5 replace semantics: emit the partial snapshot directly. With a
      // REST snapshot sidecar, the diff book is anchored by those snapshots.
      // Without that sidecar, replay uses depth5 as the deterministic checkpoint
      // available in the market-data CSV itself.
      ObRow row; row.ts = ts; row.seqNo = ++seqs[symbol]; row.id = ob.book().id();
      row.type = '5'; row.side = 'N';
      for (size_t i = 0; i < ev.bids.size() && i < 5; ++i) { row.bid[i] = ev.bids[i].price; row.bid_size[i] = ev.bids[i].qty; }
      for (size_t i = 0; i < ev.asks.size() && i < 5; ++i) { row.ask[i] = ev.asks[i].price; row.ask_size[i] = ev.asks[i].qty; }
      writers.at(symbol)->write_row(row);
      m.ob_rows_written++;
      continue;  // depth5 fully handled
    } else if (kind == "trade") {
      m.msgs_trade++;
      if (cfg.integrate_trades) { emit = true; type = 'T'; }
    }

    // Fail closed: never emit from an unseeded / resync-pending book.
    if (emit && (!ob.initialized() || ob.resync_pending())) emit = false;
    if (emit) {
      ObRow row; row.ts = ts; row.seqNo = ++seqs[symbol]; row.type = type; row.side = 'N';
      ob.book().fill_top5(row);
      writers.at(symbol)->write_row(row);
      m.ob_rows_written++;
    }
  }
  for (auto& kv : writers) kv.second->flush();
  return 0;
}

}  // namespace blob
