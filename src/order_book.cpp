#include "order_book.hpp"
#include <algorithm>

namespace blob {

namespace {
constexpr size_t kTopCache = 64;

bool better_bid(int64_t a, int64_t b) { return a > b; }
bool better_ask(int64_t a, int64_t b) { return a < b; }
}  // namespace

void OrderBook::set_snapshot(const std::vector<Level>& bids,
                             const std::vector<Level>& asks,
                             uint64_t snapshot_last_update_id) {
  bids_.clear();
  asks_.clear();
  bids_.reserve(bids.size() * 2 + 16);
  asks_.reserve(asks.size() * 2 + 16);
  for (const auto& l : bids) if (l.qty != 0) bids_[l.price] = l.qty;
  for (const auto& l : asks) if (l.qty != 0) asks_[l.price] = l.qty;
  rebuild_bid_cache();
  rebuild_ask_cache();
  last_update_id_ = snapshot_last_update_id;
  initialized_ = true;
  started_ = false;
}

void OrderBook::apply_bid_level(const Level& l) {
  bool visible = remove_cache_price(bid_cache_, l.price);
  if (l.qty == 0) {
    bids_.erase(l.price);
    if (visible || bid_cache_.count < 5) rebuild_bid_cache();
    return;
  }
  bids_[l.price] = l.qty;
  upsert_bid_cache(l);
}

void OrderBook::apply_ask_level(const Level& l) {
  bool visible = remove_cache_price(ask_cache_, l.price);
  if (l.qty == 0) {
    asks_.erase(l.price);
    if (visible || ask_cache_.count < 5) rebuild_ask_cache();
    return;
  }
  asks_[l.price] = l.qty;
  upsert_ask_cache(l);
}

void OrderBook::apply_levels(const std::vector<Level>& bids, const std::vector<Level>& asks) {
  for (const auto& l : bids) apply_bid_level(l);
  for (const auto& l : asks) apply_ask_level(l);
}

ApplyResult OrderBook::apply_diff_spot(uint64_t U, uint64_t u,
                                       const std::vector<Level>& bids,
                                       const std::vector<Level>& asks) {
  if (!initialized_) return ApplyResult::NeedSnapshot;
  if (u <= last_update_id_) return ApplyResult::IgnoredOld;
  if (U > last_update_id_ + 1) return ApplyResult::Gap;
  apply_levels(bids, asks);
  last_update_id_ = u;
  started_ = true;
  return ApplyResult::Applied;
}

ApplyResult OrderBook::apply_diff_usdm(uint64_t U, uint64_t u, uint64_t pu,
                                       const std::vector<Level>& bids,
                                       const std::vector<Level>& asks) {
  if (!initialized_) return ApplyResult::NeedSnapshot;
  if (u <= last_update_id_) return ApplyResult::IgnoredOld;
  if (!started_) {
    if (U > last_update_id_) return ApplyResult::Gap;
  } else if (pu != last_update_id_) {
    return ApplyResult::Gap;
  }
  apply_levels(bids, asks);
  last_update_id_ = u;
  started_ = true;
  return ApplyResult::Applied;
}

void OrderBook::fill_top5(ObRow& row) const {
  row.id = id_;
  for (int i = 0; i < 5; ++i) {
    if ((size_t)i < bid_cache_.count) {
      row.bid[i] = bid_cache_.levels[(size_t)i].price;
      row.bid_size[i] = bid_cache_.levels[(size_t)i].qty;
    }
    if ((size_t)i < ask_cache_.count) {
      row.ask[i] = ask_cache_.levels[(size_t)i].price;
      row.ask_size[i] = ask_cache_.levels[(size_t)i].qty;
    }
  }
}

void OrderBook::reset() {
  bids_.clear();
  asks_.clear();
  clear_cache(bid_cache_);
  clear_cache(ask_cache_);
  initialized_ = false;
  last_update_id_ = 0;
  started_ = false;
}

void OrderBook::clear_cache(TopCache& c) {
  c.count = 0;
  for (auto& l : c.levels) l = Level{0, 0};
}

bool OrderBook::remove_cache_price(TopCache& c, int64_t price) {
  for (size_t i = 0; i < c.count; ++i) {
    if (c.levels[i].price != price) continue;
    for (size_t j = i + 1; j < c.count; ++j) c.levels[j - 1] = c.levels[j];
    --c.count;
    c.levels[c.count] = Level{0, 0};
    return i < 5;
  }
  return false;
}

void OrderBook::upsert_bid_cache(const Level& l) {
  if (bid_cache_.count == kTopCache &&
      !better_bid(l.price, bid_cache_.levels[kTopCache - 1].price)) return;
  size_t pos = 0;
  while (pos < bid_cache_.count && better_bid(bid_cache_.levels[pos].price, l.price)) ++pos;
  size_t limit = std::min(bid_cache_.count, kTopCache - 1);
  for (size_t j = limit; j > pos; --j) bid_cache_.levels[j] = bid_cache_.levels[j - 1];
  bid_cache_.levels[pos] = l;
  if (bid_cache_.count < kTopCache) ++bid_cache_.count;
}

void OrderBook::upsert_ask_cache(const Level& l) {
  if (ask_cache_.count == kTopCache &&
      !better_ask(l.price, ask_cache_.levels[kTopCache - 1].price)) return;
  size_t pos = 0;
  while (pos < ask_cache_.count && better_ask(ask_cache_.levels[pos].price, l.price)) ++pos;
  size_t limit = std::min(ask_cache_.count, kTopCache - 1);
  for (size_t j = limit; j > pos; --j) ask_cache_.levels[j] = ask_cache_.levels[j - 1];
  ask_cache_.levels[pos] = l;
  if (ask_cache_.count < kTopCache) ++ask_cache_.count;
}

void OrderBook::rebuild_bid_cache() {
  clear_cache(bid_cache_);
  for (const auto& kv : bids_) upsert_bid_cache(Level{kv.first, kv.second});
}

void OrderBook::rebuild_ask_cache() {
  clear_cache(ask_cache_);
  for (const auto& kv : asks_) upsert_ask_cache(Level{kv.first, kv.second});
}

}  // namespace blob
