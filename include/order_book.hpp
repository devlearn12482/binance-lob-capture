#pragma once
#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <string>
#include "types.hpp"

namespace blob {

// A single price-level update (absolute quantity at a price, both scaled).
struct Level { int64_t price; int64_t qty; };

// One emitted order-book snapshot row (top-5 each side).
struct ObRow {
  Timestamp ts;
  uint64_t  seqNo = 0;
  int32_t   id    = 0;
  char      type  = '?';
  char      side  = 'N';
  int64_t   bid[5]      = {0,0,0,0,0};
  int64_t   bid_size[5] = {0,0,0,0,0};
  int64_t   ask[5]      = {0,0,0,0,0};
  int64_t   ask_size[5] = {0,0,0,0,0};
};

// Sequence-tracking result of applying a diff event.
enum class ApplyResult { Applied, IgnoredOld, Gap, NeedSnapshot };

// Local order book for one symbol.
//
// Storage: full-depth hash maps plus fixed top caches. Hash maps give average
// O(1) level updates; cached top levels make row emission O(1). Rare cache
// underflow repairs scan full depth to preserve correctness after visible
// deletes.
class OrderBook {
public:
  explicit OrderBook(int32_t instrument_id) : id_(instrument_id) {}

  // Sequence state -----------------------------------------------------------
  bool      initialized() const { return initialized_; }
  uint64_t  last_update_id() const { return last_update_id_; }
  int32_t   id() const { return id_; }

  // Seed the book from a REST/partial snapshot (absolute levels).
  void set_snapshot(const std::vector<Level>& bids,
                    const std::vector<Level>& asks,
                    uint64_t snapshot_last_update_id);

  // Apply a SPOT diff event. Gap rule: U > book_id+1 => Gap (resync needed).
  ApplyResult apply_diff_spot(uint64_t U, uint64_t u,
                              const std::vector<Level>& bids,
                              const std::vector<Level>& asks);

  // Apply a USD-M diff event. Continuity rule: pu must equal previous u.
  ApplyResult apply_diff_usdm(uint64_t U, uint64_t u, uint64_t pu,
                              const std::vector<Level>& bids,
                              const std::vector<Level>& asks);

  // Fill the top-5 fields of a row from current state.
  void fill_top5(ObRow& row) const;

  void reset();

private:
  struct TopCache {
    std::array<Level, 64> levels{};
    size_t count = 0;
  };

  void apply_levels(const std::vector<Level>& bids, const std::vector<Level>& asks);
  void apply_bid_level(const Level& l);
  void apply_ask_level(const Level& l);
  void clear_cache(TopCache& c);
  void rebuild_bid_cache();
  void rebuild_ask_cache();
  void upsert_bid_cache(const Level& l);
  void upsert_ask_cache(const Level& l);
  bool remove_cache_price(TopCache& c, int64_t price);

  int32_t id_;
  bool     initialized_   = false;
  uint64_t last_update_id_ = 0;
  bool     started_       = false;  // an event applied since last snapshot
  std::unordered_map<int64_t,int64_t> bids_;  // price -> qty
  std::unordered_map<int64_t,int64_t> asks_;  // price -> qty
  TopCache bid_cache_;
  TopCache ask_cache_;
};

}  // namespace blob
