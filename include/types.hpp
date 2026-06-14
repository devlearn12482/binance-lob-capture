#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace blob {

// Fixed-point scaling. Binance sends decimals as strings; we convert to scaled
// int64 deterministically (no binary float in the hot path / CSV columns).
inline constexpr int     kPriceScalePow = 8;          // price * 10^8
inline constexpr int     kQtyScalePow   = 8;          // qty   * 10^8
inline constexpr int64_t kPriceScale    = 100000000;  // 10^8
inline constexpr int64_t kQtyScale      = 100000000;  // 10^8

enum class Venue : uint8_t { Spot = 0, Usdm = 1 };

// stream_kind values for the market-data CSV.
enum class StreamKind : uint8_t { DepthDiff = 0, Depth5 = 1, Trade = 2 };

inline const char* to_string(Venue v) { return v == Venue::Spot ? "spot" : "usdm"; }
inline const char* to_string(StreamKind k) {
  switch (k) {
    case StreamKind::DepthDiff: return "depth_diff";
    case StreamKind::Depth5:    return "depth5";
    case StreamKind::Trade:     return "trade";
  }
  return "?";
}

// Single ASCII letter recorded in the order-book CSV 'type' column.
//   D = applied depthUpdate (diff)
//   S = depth5 partial-snapshot refresh
//   T = trade annotation (only if trade integration enabled)
inline char type_letter(StreamKind k) {
  switch (k) {
    case StreamKind::DepthDiff: return 'D';
    case StreamKind::Depth5:    return 'S';
    case StreamKind::Trade:     return 'T';
  }
  return '?';
}

// side column: B bid-side, S ask-side, N symmetric/NA
enum class Side : char { Bid = 'B', Ask = 'S', NA = 'N' };

struct Timestamp {
  int64_t tsec  = 0;  // integer seconds
  int32_t tnsec = 0;  // [0, 999_999_999]
};

// Parse a decimal string (e.g. "0.00240000") into a scaled int64 without going
// through double. Returns false on malformed input.
[[nodiscard]] bool parse_scaled(const char* s, size_t len, int scale_pow, int64_t& out);
[[nodiscard]] inline bool parse_scaled(const std::string& s, int scale_pow, int64_t& out) {
  return parse_scaled(s.data(), s.size(), scale_pow, out);
}

}  // namespace blob
