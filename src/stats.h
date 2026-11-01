#pragma once
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <algorithm>
#include <stdlib.h>

namespace cst {

struct Stat { std::string cmd; long long count = 0, total = 0, max = 0; };

inline std::mutex& Mu() { static std::mutex m; return m; }
inline std::map<std::string, Stat>& Tab() { static std::map<std::string, Stat> t; return t; }

inline long long SlowMs() {
  static long long v = -1;
  if (v < 0) { const char* e = getenv("ANOMALY_SLOW_MS"); v = e && *e ? atoll(e) : 250; }
  return v;
}

inline void Add(const std::string& cmd, long long ms) {
  if (cmd.empty()) return;
  std::lock_guard<std::mutex> l(Mu());
  Stat& s = Tab()[cmd]; s.cmd = cmd; s.count++; s.total += ms; s.max = std::max(s.max, ms);
}

inline void Clear() { std::lock_guard<std::mutex> l(Mu()); Tab().clear(); }

inline std::vector<Stat> Rows() {
  std::vector<Stat> r;
  { std::lock_guard<std::mutex> l(Mu()); for (auto& e : Tab()) r.push_back(e.second); }
  std::sort(r.begin(), r.end(), [](const Stat& a, const Stat& b) { return a.total != b.total ? a.total > b.total : a.cmd < b.cmd; });
  return r;
}

}
