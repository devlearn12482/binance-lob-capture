#pragma once
#include <cstddef>
#include <deque>
#include <vector>
#include "json_min.hpp"
#include "order_book.hpp"
#include "types.hpp"

namespace blob {

// Wraps an OrderBook with the official Binance "manage a local order book"
// algorithm: buffer diffs until a REST snapshot arrives, drop stale buffered
// events, seed the book, then apply live. Handles spot (U/u) and USD-M
// (U/u/pu) sequencing and triggers a resync on a detected gap.
//
// Network is injected: the caller supplies the snapshot (so this is fully
// unit-testable and usable in replay without sockets).
class BookSession {
public:
  BookSession(int32_t id, Venue venue) : ob_(id), venue_(venue) {}

  enum class Outcome { Buffered, Applied, ResyncNeeded, Dropped };

  bool initialized() const { return ob_.initialized(); }
  const OrderBook& book() const { return ob_; }
  OrderBook& book() { return ob_; }
  bool resync_pending() const { return resync_pending_; }
  uint64_t buffer_overflows() const { return overflows_; }
  size_t buffered() const { return buffer_.size(); }

  // Feed one diff event. Before a snapshot is set, events are buffered.
  Outcome on_diff(const DepthEvent& ev);

  // Apply the REST/partial snapshot (absolute levels + its lastUpdateId), then
  // replay buffered events per the algorithm. Returns number of buffered events
  // applied.
  size_t on_snapshot(const std::vector<Level>& bids,
                     const std::vector<Level>& asks,
                     uint64_t snapshot_last_update_id);

  // Mark that a resync is required (e.g. on reconnect/new epoch).
  void request_resync() { resync_pending_ = true; ob_.reset(); buffer_.clear(); }

private:
  Outcome apply_one(const DepthEvent& ev);
  void buffer_event(const DepthEvent& ev);

  // Cap on diffs buffered while waiting for a snapshot. ~100ms streams produce a
  // handful of diffs per resync; a persistent snapshot failure must not grow the
  // buffer without bound. On overflow we drop the backlog and stay pending.
  static constexpr size_t kMaxBuffer = 1u << 16;  // 65536 events

  OrderBook ob_;
  Venue venue_;
  bool resync_pending_ = true;        // need a snapshot before live apply
  uint64_t overflows_ = 0;            // times the buffer cap was hit
  std::deque<DepthEvent> buffer_;     // diffs seen before/while snapshotting
};

}  // namespace blob
