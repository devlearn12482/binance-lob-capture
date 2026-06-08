#pragma once
#include "config.hpp"
#include "metrics.hpp"

namespace blob {
// Replay mode: read a saved market_data_*.csv and regenerate the
// *_orderbook.csv with no network. Returns process exit code.
int run_replay(const Config& cfg, Metrics& m);
}  // namespace blob
