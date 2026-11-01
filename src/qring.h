#pragma once
#include <stdint.h>
#include <string>
#include <vector>
#include <mutex>

namespace qr {

struct Ev { uint64_t seq = 0; uint64_t ms = 0; uint32_t type = 0; std::string guid, sid; int found = -1; std::string kind; };

class Ring {
 public:
  explicit Ring(size_t cap) : cap_(cap ? cap : 1) { v_.resize(cap_); }
  uint64_t Push(Ev e) {
    std::lock_guard<std::mutex> l(m_);
    e.seq = ++seq_;
    v_[(e.seq - 1) % cap_] = e;
    return e.seq;
  }
  uint64_t Seq() { std::lock_guard<std::mutex> l(m_); return seq_; }
  std::vector<Ev> Since(uint64_t since, const std::string& sidSub = "", size_t lim = 200) {
    std::lock_guard<std::mutex> l(m_);
    std::vector<Ev> out;
    uint64_t lo = seq_ > cap_ ? seq_ - cap_ : 0;
    if (since > lo) lo = since;
    for (uint64_t s = lo + 1; s <= seq_; s++) {
      const Ev& e = v_[(s - 1) % cap_];
      if (!sidSub.empty() && e.sid.find(sidSub) == std::string::npos) continue;
      out.push_back(e);
    }
    if (out.size() > lim) out.erase(out.begin(), out.end() - lim);
    return out;
  }
  void Clear() { std::lock_guard<std::mutex> l(m_); seq_ = 0; }
 private:
  std::mutex m_;
  std::vector<Ev> v_;
  size_t cap_;
  uint64_t seq_ = 0;
};

}
