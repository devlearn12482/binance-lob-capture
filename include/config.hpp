#pragma once
#include <string>
#include <vector>
#include "types.hpp"

namespace blob {

// Time policy for recv timestamps written to both CSVs.
enum class TimePolicy { RecvWallClock, BinanceEventTime };

struct Config {
  Venue              venue       = Venue::Spot;
  std::vector<std::string> symbols;          // uppercase, e.g. {"BTCUSDT"}
  std::string        output_dir  = "./output";
  int                duration_s  = 0;         // 0 = run until SIGINT
  TimePolicy         time_policy = TimePolicy::RecvWallClock;
  bool               integrate_trades = false; // annotate OB rows on trades
  bool               sync_writes = false;     // write CSVs inline (no writer thread)
  int                shard_id    = 0;
  int                shards      = 1;         // number of WebSocket connections

  // Replay mode: regenerate *_orderbook.csv from a saved market_data_*.csv,
  // no network. Empty = live mode.
  std::string        replay_input;

  // Returns the combined-stream base URL for the venue.
  std::string ws_base_url() const;
  // Builds the combined ?streams= query for all symbols (depth/depth5/trade).
  std::string combined_streams() const;

  static Config from_args(int argc, char** argv, std::string& err);
  static const char* usage();
};

// Partition symbols across N shards, round-robin. Returns a vector of subsets
// (empty subsets omitted). Used to drive one WebSocket connection per shard.
std::vector<std::vector<std::string>> shard_symbols(
    const std::vector<std::string>& symbols, int shards);

}  // namespace blob
