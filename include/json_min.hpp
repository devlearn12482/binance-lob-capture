#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "order_book.hpp"
#include "types.hpp"

namespace blob {

// Minimal, allocation-light extractor for the *known* Binance payload shapes.
// NOT a general JSON parser. Kept dependency-free so the project always builds;
// the README documents swapping in simdjson for the hot path. Operates on the
// inner 'data' object (envelope already stripped).
//
// Parsed diff/partial-depth fields (absent fields left at default).
struct DepthEvent {
  uint64_t U = 0, u = 0, pu = 0;
  bool has_pu = false;
  uint64_t lastUpdateId = 0;   // depth5 partial snapshot
  int64_t event_time_ms = 0;   // Binance E/T event time, 0 when absent
  std::vector<Level> bids;
  std::vector<Level> asks;
};

// Clear parsed fields while retaining vector capacity for hot-path reuse.
void reset_depth_event(DepthEvent& ev);

// Split a combined-stream frame {"stream":..,"data":{..}} into its parts.
// Returns false if not a combined envelope. Reusable outputs retain capacity.
[[nodiscard]] bool split_envelope(const std::string& frame,
                                  std::string& stream_out, std::string& data_out);

// Classify by the combined-stream NAME (authoritative): "<sym>@depth5@..",
// "<sym>@depth@..", "<sym>@trade". USD-M partial depth also carries
// e:"depthUpdate", so payload-shape classification is unreliable — use this.
StreamKind classify_stream(const std::string& stream_name);

// Classify an inner payload by its "e"/keys (fallback / spot only).
StreamKind classify(const std::string& data);

// Parse a depthUpdate or depth5 inner payload into scaled levels.
[[nodiscard]] bool parse_depth(const std::string& data, DepthEvent& out);

// Binance event time in milliseconds from the inner payload ("E" field; or "T"
// trade time as fallback). Returns 0 if absent (e.g. spot depth5 has neither).
int64_t event_time_ms(const std::string& data);

}  // namespace blob
