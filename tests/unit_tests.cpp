#include "csv_writer.hpp"
#include "json_min.hpp"
#include "config.hpp"
#include "spsc_ring.hpp"
#include "book_session.hpp"
#include "order_book.hpp"
#include "types.hpp"
#include <cassert>
#include <cstdio>
#include <string>

using namespace blob;
static int failures = 0;
#define CHECK(c) do { if(!(c)){ std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } } while(0)

static void test_parse_scaled() {
  int64_t v = 0;
  CHECK(parse_scaled(std::string("0.0024"), 8, v) && v == 240000);
  CHECK(parse_scaled(std::string("100"), 8, v) && v == 10000000000LL);
  CHECK(parse_scaled(std::string("12345.67890000"), 8, v) && v == 1234567890000LL);
  CHECK(!parse_scaled(std::string("abc"), 8, v));
}

static void test_csv_escape() {
  CHECK(csv_escape("plain") == "plain");
  CHECK(csv_escape("a,b") == "\"a,b\"");
  CHECK(csv_escape("he\"llo") == "\"he\"\"llo\"");
}

static void test_envelope_and_depth() {
  std::string frame =
    "{\"stream\":\"btcusdt@depth@100ms\",\"data\":{\"e\":\"depthUpdate\",\"E\":1,\"s\":\"BTCUSDT\","
    "\"U\":157,\"u\":160,\"b\":[[\"0.0024\",\"10\"]],\"a\":[[\"0.0026\",\"100\"]]}}";
  std::string stream, data;
  CHECK(split_envelope(frame, stream, data));
  CHECK(stream == "btcusdt@depth@100ms");
  CHECK(classify(data) == StreamKind::DepthDiff);
  DepthEvent ev; CHECK(parse_depth(data, ev));
  CHECK(ev.U == 157 && ev.u == 160);
  CHECK(ev.event_time_ms == 1);
  CHECK(ev.bids.size() == 1 && ev.bids[0].price == 240000 && ev.bids[0].qty == 1000000000LL);
  CHECK(ev.asks.size() == 1 && ev.asks[0].price == 260000);
}

static void test_orderbook_diff_semantics() {
  OrderBook ob(1);
  ob.set_snapshot({{100,5},{99,3}}, {{101,4},{102,6}}, 10);
  // qty 0 removes a level; new price inserts; sequence advances
  ApplyResult r = ob.apply_diff_spot(11, 12, {{100,0},{98,7}}, {{101,9}});
  CHECK(r == ApplyResult::Applied);
  ObRow row; ob.fill_top5(row);
  CHECK(row.bid[0] == 99 && row.bid_size[0] == 3);  // 100 removed -> 99 best
  CHECK(row.bid[1] == 98 && row.bid_size[1] == 7);
  CHECK(row.ask[0] == 101 && row.ask_size[0] == 9); // 101 updated to qty 9
  // gap detection: U too far ahead
  CHECK(ob.apply_diff_spot(100, 101, {}, {}) == ApplyResult::Gap);
  // stale event ignored
  CHECK(ob.apply_diff_spot(1, 2, {}, {}) == ApplyResult::IgnoredOld);
}

static void test_usdm_continuity() {
  OrderBook ob(2);
  ob.set_snapshot({{100,1}}, {{101,1}}, 50);
  // First event must bracket the snapshot id: U <= 50 <= u.
  CHECK(ob.apply_diff_usdm(50, 55, 49, {{100,2}}, {}) == ApplyResult::Applied);
  // next pu must equal previous u (55); break it -> Gap
  CHECK(ob.apply_diff_usdm(56, 60, 99, {}, {}) == ApplyResult::Gap);
}

static void test_usdm_first_event_gap() {
  OrderBook ob(7);
  ob.set_snapshot({{100,1}}, {{101,1}}, 50);
  // First event with U > lastUpdateId (non-overlapping) must be flagged as a gap.
  CHECK(ob.apply_diff_usdm(52, 60, 0, {}, {}) == ApplyResult::Gap);
}


static DepthEvent mk(uint64_t U,uint64_t u,uint64_t pu,bool has_pu){
  DepthEvent e; e.U=U; e.u=u; e.pu=pu; e.has_pu=has_pu; return e;
}

static void test_session_resync_buffer() {
  // Spot: buffer diffs, then snapshot drops stale and applies the rest.
  BookSession s(1, Venue::Spot);
  CHECK(s.resync_pending());
  CHECK(s.on_diff(mk(5,8,0,false)) == BookSession::Outcome::Buffered);   // stale vs snapshot
  CHECK(s.on_diff(mk(9,12,0,false)) == BookSession::Outcome::Buffered);  // u=12, kept
  // snapshot lastUpdateId=10 -> first event (u=8) dropped, second (U=9<=11,u=12) applied
  size_t applied = s.on_snapshot({{100,1}},{{101,1}}, 10);
  CHECK(applied == 1);
  CHECK(s.initialized() && !s.resync_pending());
  CHECK(s.book().last_update_id() == 12);
  // a subsequent in-order diff applies live
  CHECK(s.on_diff(mk(13,13,0,false)) == BookSession::Outcome::Applied);
  // a gap triggers resync request
  CHECK(s.on_diff(mk(100,101,0,false)) == BookSession::Outcome::ResyncNeeded);
  CHECK(s.resync_pending());
}

static void test_session_usdm_gap() {
  BookSession s(2, Venue::Usdm);
  s.on_snapshot({{100,1}},{{101,1}}, 50);
  CHECK(s.on_diff(mk(50,55,49,true)) == BookSession::Outcome::Applied);   // U<=50<=u
  CHECK(s.on_diff(mk(56,60,99,true)) == BookSession::Outcome::ResyncNeeded);  // pu!=prev u
}

static void test_session_failclosed_on_gap() {
  // Buffered replay that hits a gap must leave the session resync_pending
  // (fail closed) rather than reporting ready.
  BookSession s(8, Venue::Spot);
  s.on_diff(mk(20,25,0,false));   // buffered; far ahead of the snapshot below
  s.on_snapshot({{100,1}},{{101,1}}, 5);  // snapshot id 5; buffered U=20 >> 6 -> gap
  CHECK(s.resync_pending());      // fail closed, not ready to emit
}


static void test_multi_level_parse() {
  // A single diff with multiple levels per side must parse ALL of them.
  std::string data =
    "{\"e\":\"depthUpdate\",\"U\":1,\"u\":2,"
    "\"b\":[[\"100.00\",\"1\"],[\"99.50\",\"2\"],[\"99.00\",\"3\"]],"
    "\"a\":[[\"101.00\",\"4\"],[\"101.50\",\"5\"]]}";
  DepthEvent ev; CHECK(parse_depth(data, ev));
  CHECK(ev.bids.size() == 3);
  CHECK(ev.asks.size() == 2);
  CHECK(ev.bids[0].price == 10000000000LL && ev.bids[0].qty == 100000000LL);
  CHECK(ev.bids[2].price ==  9900000000LL && ev.bids[2].qty == 300000000LL);
  CHECK(ev.asks[1].price == 10150000000LL && ev.asks[1].qty == 500000000LL);
  // Apply to a book and confirm it is NOT crossed (best bid < best ask).
  OrderBook ob(9);
  ob.set_snapshot(ev.bids, ev.asks, 2);
  ObRow row; ob.fill_top5(row);
  CHECK(row.bid[0] == 10000000000LL);
  CHECK(row.ask[0] == 10100000000LL);
  CHECK(row.bid[0] < row.ask[0]);  // not crossed
}


static void test_shard_partition() {
  auto g = shard_symbols({"A","B","C","D","E"}, 2);
  CHECK(g.size() == 2);
  CHECK(g[0].size() == 3 && g[1].size() == 2);   // round-robin: A,C,E | B,D
  CHECK(g[0][0] == "A" && g[0][1] == "C" && g[0][2] == "E");
  CHECK(g[1][0] == "B" && g[1][1] == "D");
  // more shards than symbols -> clamped, no empty trailing shards
  auto g2 = shard_symbols({"X"}, 4);
  CHECK(g2.size() == 1 && g2[0][0] == "X");
  // single shard keeps order
  auto g3 = shard_symbols({"A","B","C"}, 1);
  CHECK(g3.size() == 1 && g3[0].size() == 3);
}


static void test_spsc_ring_single_thread() {
  SpscByteRing r(8);  // 8 slots
  CHECK(r.empty());
  CHECK(r.try_write("ab", 2));
  CHECK(r.try_write("cde", 3));
  CHECK(!r.empty());
  std::string out;
  size_t n = r.drain([&](const char* p, size_t len){ out.append(p, len); out.push_back('|'); });
  CHECK(n == 2);
  CHECK(out == "ab|cde|");
  CHECK(r.empty());
}

static void test_spsc_ring_full() {
  SpscByteRing r(4);  // capacity 4 -> at most 3 in flight
  CHECK(r.try_write("1", 1));
  CHECK(r.try_write("2", 1));
  CHECK(r.try_write("3", 1));
  CHECK(!r.try_write("4", 1));   // full
  std::string out;
  r.drain([&](const char* p, size_t len){ out.append(p, len); });
  CHECK(out == "123");
  CHECK(r.try_write("4", 1));    // space again after drain
}


static void test_duplicate_ignored() {
  OrderBook ob(11);
  ob.set_snapshot({{100,1}}, {{101,1}}, 50);
  CHECK(ob.apply_diff_spot(50, 55, {{100,2}}, {}) == ApplyResult::Applied);  // bridge
  CHECK(ob.apply_diff_spot(56, 55, {{100,9}}, {}) == ApplyResult::IgnoredOld); // duplicate u==last
  ObRow r; ob.fill_top5(r);
  CHECK(r.bid_size[0] == 2);  // duplicate did not change state
}

static void test_snapshot_boundary_stale_ignored() {
  OrderBook spot(12);
  spot.set_snapshot({{100,1}}, {{101,1}}, 50);
  CHECK(spot.apply_diff_spot(49, 50, {{100,9}}, {}) == ApplyResult::IgnoredOld);
  ObRow sr; spot.fill_top5(sr);
  CHECK(sr.bid_size[0] == 1);

  OrderBook usdm(13);
  usdm.set_snapshot({{100,1}}, {{101,1}}, 50);
  CHECK(usdm.apply_diff_usdm(49, 50, 49, {{100,9}}, {}) == ApplyResult::IgnoredOld);
  ObRow ur; usdm.fill_top5(ur);
  CHECK(ur.bid_size[0] == 1);
}

static void test_precision_reject() {
  int64_t v = 0;
  CHECK(parse_scaled(std::string("0.00000001"), 8, v) && v == 1);       // exactly 8 dp ok
  CHECK(!parse_scaled(std::string("0.000000001"), 8, v));               // 9th nonzero dp rejected
  CHECK(parse_scaled(std::string("1.230000000"), 8, v) && v == 123000000); // trailing zeros ok
}


static void test_gap_event_buffered() {
  // A diff that detects a gap must be kept and replayed by the next snapshot.
  BookSession s(21, Venue::Spot);
  s.on_snapshot({{100,1}}, {{101,1}}, 50);
  CHECK(s.on_diff(mk(51,55,0,false)) == BookSession::Outcome::Applied);
  // gap: U=100 is far ahead of book id 55 -> ResyncNeeded, and must be buffered
  DepthEvent gap; gap.U = 100; gap.u = 105; gap.bids = {{100, 7}};
  CHECK(s.on_diff(gap) == BookSession::Outcome::ResyncNeeded);
  CHECK(s.resync_pending());
  // a snapshot slightly BEHIND the gap event (id 99) must replay it (U=100<=99+1)
  size_t applied = s.on_snapshot({{100,1}}, {{101,1}}, 99);
  CHECK(applied == 1);                       // the buffered gap event was applied
  CHECK(!s.resync_pending());
  CHECK(s.book().last_update_id() == 105);
  ObRow r; s.book().fill_top5(r);
  CHECK(r.bid_size[0] == 7);                  // gap event's update is present
}


static void test_invalid_level_rejected() {
  DepthEvent ok; 
  CHECK(parse_depth(std::string("{\"U\":1,\"u\":2,\"b\":[[\"100.00\",\"1\"]],\"a\":[]}"), ok));
  DepthEvent bad_price;
  CHECK(!parse_depth(std::string("{\"U\":1,\"u\":2,\"b\":[[\"abc\",\"1\"]],\"a\":[]}"), bad_price));
  DepthEvent bad_qty;
  CHECK(!parse_depth(std::string("{\"U\":1,\"u\":2,\"b\":[[\"100.00\",\"x\"]],\"a\":[]}"), bad_qty));
}


static void test_buffer_bounded() {
  // While stuck resync-pending, the buffer must stay bounded and count overflows.
  BookSession s(30, Venue::Spot);  // starts resync_pending (no snapshot yet)
  for (int i = 0; i < 70000; ++i) s.on_diff(mk((uint64_t)i+1, (uint64_t)i+1, 0, false));
  CHECK(s.buffered() <= (1u << 16));
  CHECK(s.buffer_overflows() >= 1);
}


static void test_gap_suffix_preserved() {
  // Buffer two events with a gap between them. The first snapshot bridges only
  // the first; the second (gap-triggering) event must be KEPT for the next
  // snapshot, not discarded.
  BookSession s(31, Venue::Spot);          // starts resync_pending
  s.on_diff(mk(100, 105, 0, false));       // evA buffered
  s.on_diff(mk(200, 205, 0, false));       // evB buffered (gap vs evA)
  size_t a1 = s.on_snapshot({{100,1}}, {{101,1}}, 99);
  CHECK(a1 == 1);                          // only evA applied
  CHECK(s.resync_pending());               // evB caused a gap -> still pending
  CHECK(s.buffered() == 1);                // evB preserved (suffix kept)
  size_t a2 = s.on_snapshot({{100,1}}, {{101,1}}, 199);
  CHECK(a2 == 1);                          // evB now applied
  CHECK(!s.resync_pending());
  CHECK(s.book().last_update_id() == 205);
  CHECK(s.buffered() == 0);
}


static void test_classify_stream() {
  // Stream name is authoritative (USD-M depth5 payload also has e:depthUpdate).
  CHECK(classify_stream("btcusdt@depth5@100ms") == StreamKind::Depth5);
  CHECK(classify_stream("btcusdt@depth@100ms")  == StreamKind::DepthDiff);
  CHECK(classify_stream("btcusdt@trade")        == StreamKind::Trade);
  CHECK(classify_stream("ethusdt@depth5")       == StreamKind::Depth5);  // depth5 before depth
}

int main() {
  test_parse_scaled();
  test_csv_escape();
  test_envelope_and_depth();
  test_orderbook_diff_semantics();
  test_usdm_continuity();
  test_session_resync_buffer();
  test_session_usdm_gap();
  test_usdm_first_event_gap();
  test_session_failclosed_on_gap();
  test_multi_level_parse();
  test_shard_partition();
  test_spsc_ring_single_thread();
  test_spsc_ring_full();
  test_duplicate_ignored();
  test_snapshot_boundary_stale_ignored();
  test_precision_reject();
  test_gap_event_buffered();
  test_invalid_level_rejected();
  test_buffer_bounded();
  test_gap_suffix_preserved();
  test_classify_stream();
  if (failures == 0) std::printf("ALL TESTS PASSED\n");
  else std::printf("%d CHECK(S) FAILED\n", failures);
  return failures == 0 ? 0 : 1;
}
