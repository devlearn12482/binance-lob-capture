#pragma once
#include <string>
#include "json_min.hpp"
#include "types.hpp"

namespace blob {

// Fetch a REST depth snapshot for proper LOB resync (Binance "manage a local
// order book" step). Fills out.bids/out.asks (scaled) and out.lastUpdateId.
//   spot: GET https://api.binance.com/api/v3/depth?symbol=SYM&limit=<=5000
//   usdm: GET https://fapi.binance.com/fapi/v1/depth?symbol=SYM&limit=<=1000
// Returns false (with err set) if not built with ENABLE_LIVE or on HTTP error.
[[nodiscard]] bool fetch_depth_snapshot(Venue venue, const std::string& symbol, int limit,
                                        DepthEvent& out, std::string& err,
                                        std::string* raw_json = nullptr);

}  // namespace blob
