#include "book_session.hpp"

namespace blob {

BookSession::Outcome BookSession::apply_one(const DepthEvent& ev) {
  ApplyResult r = (venue_ == Venue::Usdm)
      ? ob_.apply_diff_usdm(ev.U, ev.u, ev.pu, ev.bids, ev.asks)
      : ob_.apply_diff_spot(ev.U, ev.u, ev.bids, ev.asks);
  switch (r) {
    case ApplyResult::Applied:      return Outcome::Applied;
    case ApplyResult::IgnoredOld:   return Outcome::Dropped;
    case ApplyResult::Gap:          resync_pending_ = true; return Outcome::ResyncNeeded;
    case ApplyResult::NeedSnapshot: resync_pending_ = true; return Outcome::Buffered;
  }
  return Outcome::Dropped;
}

void BookSession::buffer_event(const DepthEvent& ev) {
  if (buffer_.size() >= kMaxBuffer) {
    // Snapshot is not arriving (persistent failure). Drop the backlog and reset
    // so the next snapshot reseeds from scratch; keeps memory bounded.
    ++overflows_;
    buffer_.clear();
    resync_pending_ = true;
    ob_.reset();
  }
  buffer_.push_back(ev);
}

BookSession::Outcome BookSession::on_diff(const DepthEvent& ev) {
  if (resync_pending_ || !ob_.initialized()) {
    buffer_event(ev);                 // buffer (bounded) until snapshot seeds book
    return Outcome::Buffered;
  }
  Outcome o = apply_one(ev);
  if (o == Outcome::ResyncNeeded) {
    // The event that detected the gap must be kept: the upcoming REST snapshot
    // may be behind it, in which case on_snapshot() replays it (or drops it if
    // u <= lastUpdateId). Without this, that update would be lost.
    buffer_event(ev);
  }
  return o;
}

size_t BookSession::on_snapshot(const std::vector<Level>& bids,
                                const std::vector<Level>& asks,
                                uint64_t snapshot_last_update_id,
                                const AppliedCallback& on_applied) {
  ob_.set_snapshot(bids, asks, snapshot_last_update_id);
  resync_pending_ = false;
  size_t applied = 0;
  // Consume the buffer from the front: drop events fully older than the
  // snapshot, apply the rest. If a gap is detected mid-replay, STOP and KEEP the
  // unprocessed suffix (the gap-triggering event and everything after it) so the
  // next snapshot attempt can use it. apply_one sets resync_pending_, so the
  // caller stays fail-closed until a clean seed.
  while (!buffer_.empty()) {
    const DepthEvent& ev = buffer_.front();
    if (ev.u <= snapshot_last_update_id) { buffer_.pop_front(); continue; }  // stale
    Outcome o = apply_one(ev);
    if (o == Outcome::ResyncNeeded) break;             // gap -> preserve suffix
    if (o == Outcome::Applied) {
      ++applied;
      if (on_applied) on_applied(ev, ob_);
    }
    buffer_.pop_front();                                // applied or dropped
  }
  return applied;
}

}  // namespace blob
