#include "config.hpp"
#include <cstring>
#include <sstream>
#include <algorithm>

namespace blob {

const char* Config::usage() {
  return
  "Usage: binance_capture [options]\n"
  "  --venue spot|usdm           Market venue (default spot)\n"
  "  --symbols SYM[,SYM...]      Uppercase symbols, e.g. BTCUSDT,ETHUSDT\n"
  "  --output-dir DIR            Output directory (default ./output)\n"
  "  --duration SECONDS          Capture duration; 0 = until SIGINT (default 0)\n"
  "  --shard-id N                Shard index for this connection (default 0)\n"
  "  --shards N                  Spread symbols across N connections (default 1)\n"
  "  --time-policy recv|event    Timestamp source (default recv)\n"
  "  --integrate-trades          Emit an annotated OB row on each trade\n"
  "  --sync-writes               Write CSVs inline instead of a writer thread\n"
  "  --replay FILE               Replay a saved market_data_*.csv (no network)\n"
  "  --help                      Show this help\n";
}

static std::vector<std::string> split_csv(const std::string& s) {
  std::vector<std::string> out; std::stringstream ss(s); std::string tok;
  while (std::getline(ss, tok, ',')) if (!tok.empty()) out.push_back(tok);
  return out;
}

Config Config::from_args(int argc, char** argv, std::string& err) {
  Config c;
  auto need = [&](int& i) -> const char* {
    if (i + 1 >= argc) { err = "missing value for "; err += argv[i]; return nullptr; }
    return argv[++i];
  };
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--venue") { const char* v = need(i); if (!v) return c;
      if (!std::strcmp(v, "spot")) c.venue = Venue::Spot;
      else if (!std::strcmp(v, "usdm")) c.venue = Venue::Usdm;
      else { err = "invalid --venue"; return c; }
    } else if (a == "--symbols") { const char* v = need(i); if (!v) return c;
      c.symbols = split_csv(v);
      for (auto& s : c.symbols) std::transform(s.begin(), s.end(), s.begin(), ::toupper);
    } else if (a == "--output-dir") { const char* v = need(i); if (!v) return c; c.output_dir = v;
    } else if (a == "--duration") { const char* v = need(i); if (!v) return c; c.duration_s = std::atoi(v);
    } else if (a == "--shard-id") { const char* v = need(i); if (!v) return c; c.shard_id = std::atoi(v);
    } else if (a == "--shards") { const char* v = need(i); if (!v) return c; c.shards = std::atoi(v); if (c.shards < 1) c.shards = 1;
    } else if (a == "--time-policy") { const char* v = need(i); if (!v) return c;
      c.time_policy = std::strcmp(v, "event") == 0 ? TimePolicy::BinanceEventTime : TimePolicy::RecvWallClock;
    } else if (a == "--integrate-trades") { c.integrate_trades = true;
    } else if (a == "--sync-writes") { c.sync_writes = true;
    } else if (a == "--replay") { const char* v = need(i); if (!v) return c; c.replay_input = v;
    } else if (a == "--help" || a == "-h") { err = "help"; return c;
    } else { err = "unknown argument: " + a; return c; }
  }
  // Dedup symbols (preserve order) so each symbol is owned by exactly one shard.
  {
    std::vector<std::string> uniq;
    for (const auto& sy : c.symbols)
      if (std::find(uniq.begin(), uniq.end(), sy) == uniq.end()) uniq.push_back(sy);
    c.symbols.swap(uniq);
  }
  if (c.replay_input.empty() && c.symbols.empty()) err = "at least one --symbols required (or use --replay)";
  if (err.empty() && c.replay_input.empty()) {
    const size_t shard_count = std::min(c.symbols.size(), (size_t)c.shards);
    const size_t max_symbols_per_shard =
        (c.symbols.size() + shard_count - 1) / shard_count;
    if (max_symbols_per_shard * kStreamsPerSymbol > kMaxCombinedStreams) {
      err = "too many symbols per shard: Binance permits at most " +
            std::to_string(kMaxCombinedStreams) +
            " combined streams per connection; increase --shards";
    }
  }
  return c;
}

std::string Config::ws_base_url() const {
  return venue == Venue::Spot
    ? "wss://stream.binance.com:9443/stream?streams="
    : "wss://fstream.binance.com/public/stream?streams=";
}

std::string Config::combined_streams() const {
  std::string out;
  for (size_t s = 0; s < symbols.size(); ++s) {
    std::string sym = symbols[s];
    std::transform(sym.begin(), sym.end(), sym.begin(), ::tolower);
    if (s) out += '/';
    out += sym + "@depth@100ms/" + sym + "@depth5@100ms/" + sym + "@trade";
  }
  return out;
}

std::vector<std::vector<std::string>> shard_symbols(
    const std::vector<std::string>& symbols, int shards) {
  if (shards < 1) shards = 1;
  if ((size_t)shards > symbols.size()) shards = symbols.empty() ? 1 : (int)symbols.size();
  std::vector<std::vector<std::string>> out((size_t)shards);
  for (size_t i = 0; i < symbols.size(); ++i)
    out[i % (size_t)shards].push_back(symbols[i]);
  return out;
}

}  // namespace blob
