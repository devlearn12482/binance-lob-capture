#include "json_min.hpp"
#ifdef USE_SIMDJSON
#include <simdjson.h>
#endif
#include <cctype>
#include <cstdint>
#include <cstring>

namespace blob {

void reset_depth_event(DepthEvent& ev) {
  ev.U = 0;
  ev.u = 0;
  ev.pu = 0;
  ev.has_pu = false;
  ev.lastUpdateId = 0;
  ev.event_time_ms = 0;
  ev.bids.clear();
  ev.asks.clear();
}

// ---- parse_scaled (declared in types.hpp) ---------------------------------
bool parse_scaled(const char* s, size_t len, int scale_pow, int64_t& out) {
  if (len == 0) return false;
  size_t i = 0;
  bool neg = false;
  if (s[i] == '-') { neg = true; ++i; }
  int64_t intpart = 0;
  bool any = false;
  for (; i < len && s[i] != '.'; ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    intpart = intpart * 10 + (s[i] - '0');
    any = true;
  }
  int64_t frac = 0;
  int fracdigits = 0;
  if (i < len && s[i] == '.') {
    ++i;
    for (; i < len; ++i) {
      if (s[i] < '0' || s[i] > '9') return false;
      if (fracdigits < scale_pow) { frac = frac * 10 + (s[i] - '0'); ++fracdigits; }
      else if (s[i] != '0') return false;
      any = true;
    }
  }
  if (!any) return false;
  int64_t scale = 1;
  for (int k = 0; k < scale_pow; ++k) scale *= 10;
  int64_t fscale = 1;
  for (int k = 0; k < fracdigits; ++k) fscale *= 10;
  __int128 v128 = (__int128)intpart * scale + (fscale ? (__int128)frac * (scale / fscale) : 0);
  if (v128 > (__int128)INT64_MAX) return false;
  int64_t val = (int64_t)v128;
  out = neg ? -val : val;
  return true;
}

// ---- tiny JSON helpers (purpose-built for known Binance shapes) -----------
namespace {

// Find "key": return index past the colon, or npos. Stack-built pattern — zero alloc.
size_t find_key(const std::string& s, const char* key, size_t from = 0) {
  char pat[32];
  pat[0] = '"';
  size_t klen = strlen(key);
  memcpy(pat + 1, key, klen);
  pat[1 + klen] = '"';
  size_t patlen = klen + 2;
  size_t p = s.find(pat, from, patlen);
  if (p == std::string::npos) return std::string::npos;
  p += patlen;
  while (p < s.size() && (s[p] == ' ' || s[p] == ':')) ++p;
  return p;
}

uint64_t read_u64(const std::string& s, size_t p) {
  uint64_t v = 0;
  while (p < s.size() && s[p] >= '0' && s[p] <= '9') { v = v * 10 + (uint64_t)(s[p] - '0'); ++p; }
  return v;
}

#ifndef USE_SIMDJSON
bool key_eq(const std::string& s, size_t start, size_t len, const char* key) {
  size_t klen = strlen(key);
  return len == klen && std::memcmp(s.data() + start, key, klen) == 0;
}

size_t read_levels(const std::string& s, size_t p, std::vector<Level>& out, bool& ok) {
  if (p >= s.size() || s[p] != '[') return p;
  ++p;
  while (p < s.size() && s[p] != ']') {
    if (s[p] != '[') { ++p; continue; }
    ++p;
    while (p < s.size() && s[p] != '"') ++p;
    if (p == s.size()) { ok = false; return p; }
    size_t ps = ++p;
    while (p < s.size() && s[p] != '"') ++p;
    if (p == s.size()) { ok = false; return p; }
    size_t pe = p++;
    while (p < s.size() && s[p] != '"') ++p;
    if (p == s.size()) { ok = false; return p; }
    size_t qs = ++p;
    while (p < s.size() && s[p] != '"') ++p;
    if (p == s.size()) { ok = false; return p; }
    size_t qe = p++;
    int64_t price = 0, qty = 0;
    bool pok = parse_scaled(s.data() + ps, pe - ps, kPriceScalePow, price);
    bool qok = parse_scaled(s.data() + qs, qe - qs, kQtyScalePow, qty);
    if (pok && qok) out.push_back(Level{price, qty});
    else ok = false;
    while (p < s.size() && s[p] != ']') ++p;
    if (p < s.size()) ++p;
  }
  if (p < s.size() && s[p] == ']') ++p;
  return p;
}
#endif  // !USE_SIMDJSON
}  // namespace

bool split_envelope(const std::string& frame, std::string& stream_out, std::string& data_out) {
  size_t sp = find_key(frame, "stream");
  size_t dp = find_key(frame, "data");
  if (sp == std::string::npos || dp == std::string::npos) return false;
  if (frame[sp] == '"') {
    size_t a = sp + 1, b = frame.find('"', a);
    if (b == std::string::npos) return false;
    stream_out.assign(frame.data() + a, b - a);
  }
  if (frame[dp] != '{') return false;
  int depth = 0; size_t i = dp;
  for (; i < frame.size(); ++i) {
    if (frame[i] == '{') ++depth;
    else if (frame[i] == '}') { if (--depth == 0) { ++i; break; } }
  }
  if (depth != 0) return false;
  data_out.assign(frame.data() + dp, i - dp);
  return true;
}

StreamKind classify_stream(const std::string& s) {
  if (s.find("@depth5") != std::string::npos) return StreamKind::Depth5;
  if (s.find("@trade")  != std::string::npos) return StreamKind::Trade;
  if (s.find("@depth")  != std::string::npos) return StreamKind::DepthDiff;
  return StreamKind::DepthDiff;
}

StreamKind classify(const std::string& data) {
  size_t e = find_key(data, "e");
  if (e != std::string::npos && data[e] == '"') {
    if (data.compare(e, 13, "\"depthUpdate\"") == 0) return StreamKind::DepthDiff;
    if (data.compare(e, 7,  "\"trade\"") == 0)       return StreamKind::Trade;
  }
  if (find_key(data, "lastUpdateId") != std::string::npos) return StreamKind::Depth5;
  return StreamKind::Depth5;
}

#ifdef USE_SIMDJSON
namespace {
thread_local simdjson::dom::parser g_dom_parser;

inline void simd_load_levels(simdjson::dom::element doc, const char* k_diff,
                             const char* k_snap, std::vector<Level>& dst, bool& ok) {
  using namespace simdjson;
  dom::array arr;
  if (doc[k_diff].get_array().get(arr) && doc[k_snap].get_array().get(arr)) return;
  for (dom::element lvl : arr) {
    dom::array pq;
    if (lvl.get_array().get(pq)) continue;
    std::string_view ps, qs;
    if (pq.at(0).get_string().get(ps)) continue;
    if (pq.at(1).get_string().get(qs)) continue;
    int64_t price = 0, qty = 0;
    if (parse_scaled(ps.data(), ps.size(), kPriceScalePow, price) &&
        parse_scaled(qs.data(), qs.size(), kQtyScalePow, qty))
      dst.push_back(Level{price, qty});
    else ok = false;
  }
}
}  // namespace

bool parse_depth(const std::string& data, DepthEvent& out) {
  using namespace simdjson;
  dom::element doc;
  if (g_dom_parser.parse(data).get(doc)) return false;
  int64_t v;
  if (!doc["E"].get_int64().get(v))  out.event_time_ms = v;
  if (out.event_time_ms == 0 && !doc["T"].get_int64().get(v)) out.event_time_ms = v;
  if (!doc["U"].get_int64().get(v))  out.U  = (uint64_t)v;
  if (!doc["u"].get_int64().get(v))  out.u  = (uint64_t)v;
  if (!doc["pu"].get_int64().get(v)) { out.pu = (uint64_t)v; out.has_pu = true; }
  if (!doc["lastUpdateId"].get_int64().get(v)) out.lastUpdateId = (uint64_t)v;
  bool ok = true;
  simd_load_levels(doc, "b", "bids", out.bids, ok);
  simd_load_levels(doc, "a", "asks", out.asks, ok);
  if (!ok) return false;
  return out.u != 0 || out.lastUpdateId != 0 || !out.bids.empty() || !out.asks.empty();
}
#else
bool parse_depth(const std::string& data, DepthEvent& out) {
  bool ok = true;
  for (size_t i = 0; i < data.size();) {
    size_t q = data.find('"', i);
    if (q == std::string::npos) break;
    size_t key_start = q + 1;
    size_t key_end = data.find('"', key_start);
    if (key_end == std::string::npos) break;
    size_t colon = key_end + 1;
    while (colon < data.size() && data[colon] == ' ') ++colon;
    if (colon >= data.size() || data[colon] != ':') {
      i = key_end + 1;
      continue;  // quoted price/qty strings are array values, not keys
    }
    size_t p = colon + 1;
    while (p < data.size() && data[p] == ' ') ++p;
    size_t len = key_end - key_start;

    if (key_eq(data, key_start, len, "E")) {
      out.event_time_ms = (int64_t)read_u64(data, p);
    } else if (key_eq(data, key_start, len, "T")) {
      if (out.event_time_ms == 0) out.event_time_ms = (int64_t)read_u64(data, p);
    } else if (key_eq(data, key_start, len, "U")) {
      out.U = read_u64(data, p);
    } else if (key_eq(data, key_start, len, "u")) {
      out.u = read_u64(data, p);
    } else if (key_eq(data, key_start, len, "pu")) {
      out.pu = read_u64(data, p);
      out.has_pu = true;
    } else if (key_eq(data, key_start, len, "lastUpdateId")) {
      out.lastUpdateId = read_u64(data, p);
    } else if (key_eq(data, key_start, len, "bids") || key_eq(data, key_start, len, "b")) {
      p = read_levels(data, p, out.bids, ok);
    } else if (key_eq(data, key_start, len, "asks") || key_eq(data, key_start, len, "a")) {
      p = read_levels(data, p, out.asks, ok);
    }
    i = p > key_end ? p : key_end + 1;
  }
  if (!ok) return false;
  return out.u != 0 || out.lastUpdateId != 0 || !out.bids.empty() || !out.asks.empty();
}
#endif  // USE_SIMDJSON

int64_t event_time_ms(const std::string& data) {
  size_t p;
  if ((p = find_key(data, "E")) != std::string::npos) return (int64_t)read_u64(data, p);
  if ((p = find_key(data, "T")) != std::string::npos) return (int64_t)read_u64(data, p);
  return 0;
}

}  // namespace blob
