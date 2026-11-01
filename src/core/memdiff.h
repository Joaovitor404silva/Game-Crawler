#pragma once
#include <string>
#include <vector>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace mdf {

struct Cell { int changes = 0; uint8_t firstOld = 0, firstNew = 0, last = 0; long long firstMs = -1, lastMs = -1; };

struct Run {
  int off = 0, len = 0, changes = 0;
  std::vector<uint8_t> firstOld, firstNew, last;
  long long t0 = 0, t1 = 0;
};

inline int Feed(std::vector<Cell>& cells, const uint8_t* prev, const uint8_t* cur, size_t len, long long ms) {
  int n = 0;
  for (size_t i = 0; i < len && i < cells.size(); i++) {
    if (prev[i] == cur[i]) continue;
    Cell& c = cells[i];
    if (!c.changes) { c.firstOld = prev[i]; c.firstNew = cur[i]; c.firstMs = ms; }
    c.changes++; c.last = cur[i]; c.lastMs = ms; n++;
  }
  return n;
}

inline std::vector<Run> MergeRuns(const std::vector<Cell>& cells) {
  std::vector<Run> runs;
  for (size_t i = 0; i < cells.size();) {
    if (!cells[i].changes) { i++; continue; }
    Run r; r.off = (int)i; r.t0 = cells[i].firstMs; r.t1 = cells[i].lastMs;
    size_t j = i;
    for (; j < cells.size() && cells[j].changes; j++) {
      const Cell& c = cells[j];
      r.firstOld.push_back(c.firstOld); r.firstNew.push_back(c.firstNew); r.last.push_back(c.last);
      if (c.changes > r.changes) r.changes = c.changes;
      if (c.firstMs < r.t0) r.t0 = c.firstMs;
      if (c.lastMs > r.t1) r.t1 = c.lastMs;
    }
    r.len = (int)(j - i); runs.push_back(r); i = j;
  }
  return runs;
}

inline std::string HexOf(const std::vector<uint8_t>& v) {
  std::string s; char b[3];
  for (uint8_t x : v) { snprintf(b, sizeof b, "%02x", x); s += b; }
  return s;
}

inline std::string Typed(const Run& r) {
  char s[160];
  if (r.len == 4) {
    float a, b; int32_t x, y; memcpy(&a, r.firstOld.data(), 4); memcpy(&b, r.last.data(), 4); memcpy(&x, r.firstOld.data(), 4); memcpy(&y, r.last.data(), 4);
    snprintf(s, sizeof s, " as f32=%g->%g i32=%d->%d", a, b, x, y); return s;
  }
  if (r.len == 8) {
    double a, b; int64_t x, y; memcpy(&a, r.firstOld.data(), 8); memcpy(&b, r.last.data(), 8); memcpy(&x, r.firstOld.data(), 8); memcpy(&y, r.last.data(), 8);
    snprintf(s, sizeof s, " as f64=%g->%g i64=%lld->%lld", a, b, (long long)x, (long long)y); return s;
  }
  return "";
}

}
