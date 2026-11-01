#pragma once
#include <stdint.h>
#include <atomic>
#include <string>
#include <vector>
#include <utility>
#include <algorithm>
#include <functional>
#include <stdio.h>

namespace agg {

const int MAXF = 24;

struct Group {
  std::atomic<uint64_t> key{ 0 };
  std::atomic<int64_t> hits{ 0 };
  uint64_t frames[MAXF];
  int nframes = 0;
  uint64_t firstOld = 0, firstNew = 0, where = 0;
  std::atomic<uint64_t> lastOld{ 0 }, lastNew{ 0 };
  uint32_t tid = 0;
  uint64_t firstTick = 0;
  std::atomic<uint64_t> lastTick{ 0 };
};

inline uint64_t HashFrames(const uint64_t* f, int n, uint64_t salt) {
  uint64_t h = 1469598103934665603ULL ^ (salt * 0x9E3779B97F4A7C15ULL);
  for (int i = 0; i < n; i++) { h ^= f[i]; h *= 1099511628211ULL; h ^= h >> 29; }
  return h ? h : 1;
}

class Table {
 public:
  static const int CAP = 2048;
  Group* g;
  std::atomic<int> used{ 0 };
  std::atomic<int64_t> total{ 0 }, overflow{ 0 };
  Table() : g(new Group[CAP]) { lk.clear(); }
  ~Table() { delete[] g; }
  Table(const Table&) = delete;
  Table& operator=(const Table&) = delete;

  int Hit(const uint64_t* f, int n, uint64_t salt, uint64_t where, uint64_t oldv, uint64_t newv, uint32_t tid, uint64_t tick, bool* isNew) {
    if (n > MAXF) n = MAXF;
    uint64_t key = HashFrames(f, n, salt);
    total.fetch_add(1, std::memory_order_relaxed);
    if (isNew) *isNew = false;
    for (int probe = 0; probe < CAP; probe++) {
      int idx = (int)((key + (uint64_t)probe) % CAP);
      uint64_t k = g[idx].key.load(std::memory_order_acquire);
      if (k == key) {
        g[idx].hits.fetch_add(1, std::memory_order_relaxed);
        g[idx].lastOld.store(oldv, std::memory_order_relaxed); g[idx].lastNew.store(newv, std::memory_order_relaxed);
        g[idx].lastTick.store(tick, std::memory_order_relaxed);
        return idx;
      }
      if (k == 0) {
        while (lk.test_and_set(std::memory_order_acquire)) {}
        uint64_t k2 = g[idx].key.load(std::memory_order_acquire);
        if (k2 == 0) {
          Group& x = g[idx];
          for (int i = 0; i < n; i++) x.frames[i] = f[i];
          x.nframes = n; x.firstOld = oldv; x.firstNew = newv; x.where = where; x.tid = tid; x.firstTick = tick;
          x.lastOld.store(oldv); x.lastNew.store(newv); x.lastTick.store(tick); x.hits.store(1);
          x.key.store(key, std::memory_order_release);
          used.fetch_add(1);
          lk.clear(std::memory_order_release);
          if (isNew) *isNew = true;
          return idx;
        }
        lk.clear(std::memory_order_release);
        if (k2 == key) { probe--; continue; }
      }
    }
    overflow.fetch_add(1);
    return -1;
  }

  std::vector<int> Top(int n) const {
    std::vector<int> v;
    for (int i = 0; i < CAP; i++) if (g[i].key.load(std::memory_order_acquire)) v.push_back(i);
    std::sort(v.begin(), v.end(), [&](int a, int b) { int64_t ha = g[a].hits.load(), hb = g[b].hits.load(); return ha != hb ? ha > hb : a < b; });
    if (n > 0 && (int)v.size() > n) v.resize(n);
    return v;
  }
  void Clear() {
    while (lk.test_and_set(std::memory_order_acquire)) {}
    for (int i = 0; i < CAP; i++) { g[i].key.store(0); g[i].hits.store(0); }
    used.store(0); total.store(0); overflow.store(0);
    lk.clear(std::memory_order_release);
  }
 private:
  std::atomic_flag lk;
};

typedef std::function<std::string(uint64_t addr, bool isReturn)> SymFn;

inline std::string StackText(const Group& x, const SymFn& sym, int skip = 0) {
  std::string s;
  for (int i = skip; i < x.nframes; i++) { if (i > skip) s += " < "; s += sym(x.frames[i], i > 0); }
  return s;
}

inline std::vector<std::pair<uintptr_t, int>> Chunk(uintptr_t a, size_t size) {
  std::vector<std::pair<uintptr_t, int>> r;
  while (size > 0) {
    int L = 0;
    for (int c : { 8, 4, 2, 1 }) if ((size_t)c <= size && a % c == 0) { L = c; break; }
    r.push_back({ a, L }); a += L; size -= L;
  }
  return r;
}

}
