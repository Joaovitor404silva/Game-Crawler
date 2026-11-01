#pragma once
#include <stdint.h>
#include <string>
#include <vector>
#include <functional>
#include <set>
#include <algorithm>
#include "containers.h"
#include "qring.h"

namespace qst {

struct QLayout {
  uint32_t arr = 0x340, num = 0x350, stride = 0x118, sid = 0;
  uint32_t act = 0x10, actNum = 0x18, actStride = 0x18, actObj = 8;
  uint32_t done = 0xb0, doneNum = 0xb8, doneStride = 0xc;
  uint32_t mainQ = 0x410;
};

struct ANode { uint32_t h = 0; uint64_t obj = 0; };
struct Quest { uint32_t h = 0; std::vector<ANode> active; std::vector<uint32_t> done; };
using SidFn = std::function<bool(uint32_t, std::string&)>;

inline bool ReadAll(const cnt::ReadFn& rd, uint64_t sm, const QLayout& L, std::vector<Quest>& out, std::string& err, size_t maxQuests = 20000) {
  out.clear();
  uint64_t data = 0; int32_t n = 0;
  if (!sm || !rd(sm + L.arr, &data, 8) || !rd(sm + L.num, &n, 4)) { err = "cannot read quest array"; return false; }
  if (!data || n <= 0 || (size_t)n > maxQuests) { err = "bad quest set n=" + std::to_string(n); return false; }
  std::vector<uint8_t> blk((size_t)n * L.stride);
  if (!rd(data, blk.data(), blk.size())) { err = "cannot read quest block"; return false; }
  out.reserve((size_t)n);
  for (int32_t i = 0; i < n; i++) {
    const uint8_t* e = &blk[(size_t)i * L.stride];
    Quest q; memcpy(&q.h, e + L.sid, 4);
    uint64_t ad = 0, dd = 0; int32_t an = 0, dn = 0;
    memcpy(&ad, e + L.act, 8); memcpy(&an, e + L.actNum, 4); memcpy(&dd, e + L.done, 8); memcpy(&dn, e + L.doneNum, 4);
    if (ad && an > 0 && an < 5000) {
      std::vector<uint8_t> a((size_t)an * L.actStride);
      if (rd(ad, a.data(), a.size())) for (int32_t k = 0; k < an; k++) { ANode nd; memcpy(&nd.h, &a[(size_t)k * L.actStride], 4); memcpy(&nd.obj, &a[(size_t)k * L.actStride + L.actObj], 8); q.active.push_back(nd); }
    }
    if (dd && dn > 0 && dn < 40000) {
      std::vector<uint8_t> d((size_t)dn * L.doneStride);
      if (rd(dd, d.data(), d.size())) for (int32_t k = 0; k < dn; k++) { uint32_t h; memcpy(&h, &d[(size_t)k * L.doneStride], 4); q.done.push_back(h); }
    }
    out.push_back(std::move(q));
  }
  return true;
}

inline bool Sane(const std::vector<Quest>& qs, const SidFn& sidOf, std::string& why) {
  if (qs.empty()) { why = "no quests"; return false; }
  size_t named = 0, probe = std::min<size_t>(qs.size(), 40);
  for (size_t i = 0; i < probe; i++) { std::string s; if (sidOf(qs[i].h, s)) named++; }
  if (named * 2 < probe) { why = "quest SIDs do not resolve (" + std::to_string(named) + "/" + std::to_string(probe) + "), layout or name pool changed"; return false; }
  return true;
}

struct PoolSlot { bool neg = false; uint32_t chunk = 0; uint32_t entry = 0; };
constexpr uint64_t kNegPoolDelta = 0x20090;

inline bool PoolSlotOf(uint32_t handle, PoolSlot& out) {
  int32_t s = (int32_t)handle;
  if (s == 0) return false;
  uint32_t i = s < 0 ? (uint32_t)~s : (uint32_t)s;
  out.neg = s < 0; out.chunk = i >> 10; out.entry = (i & 0x3ff) << 5;
  return true;
}

struct NodeInfo { std::string state = "none"; std::string quest; uint64_t obj = 0; int active = 0, finished = 0; };

inline NodeInfo NodeState(const std::vector<Quest>& qs, const SidFn& sidOf, const std::string& sid) {
  NodeInfo r;
  for (auto& q : qs) {
    for (auto& a : q.active) { std::string s; if (sidOf(a.h, s) && s == sid) { sidOf(q.h, r.quest); r.state = "active"; r.obj = a.obj; r.active = (int)q.active.size(); r.finished = (int)q.done.size(); return r; } }
  }
  for (auto& q : qs) {
    for (uint32_t h : q.done) { std::string s; if (sidOf(h, s) && s == sid) { sidOf(q.h, r.quest); r.state = "finished"; r.active = (int)q.active.size(); r.finished = (int)q.done.size(); return r; } }
  }
  return r;
}

inline const Quest* FindQuest(const std::vector<Quest>& qs, const SidFn& sidOf, const std::string& sid) {
  for (auto& q : qs) { std::string s; if (sidOf(q.h, s) && s == sid) return &q; }
  return nullptr;
}

inline int Diff(const std::vector<Quest>& prev, const std::vector<Quest>& cur, const SidFn& sidOf, qr::Ring& ring, uint64_t ms) {
  int n = 0;
  for (auto& c : cur) {
    const Quest* p = nullptr;
    for (auto& x : prev) if (x.h == c.h) { p = &x; break; }
    std::string qs; sidOf(c.h, qs);
    std::set<uint32_t> pa, pd, ca;
    if (p) { for (auto& a : p->active) pa.insert(a.h); pd.insert(p->done.begin(), p->done.end()); }
    for (auto& a : c.active) ca.insert(a.h);
    for (auto& a : c.active) if (!pa.count(a.h)) { qr::Ev e; e.ms = ms; e.kind = "start"; e.guid = qs; sidOf(a.h, e.sid); ring.Push(e); n++; }
    if (p) {
      for (auto& a : p->active) if (!ca.count(a.h)) { qr::Ev e; e.ms = ms; e.kind = "end"; e.guid = qs; sidOf(a.h, e.sid); ring.Push(e); n++; }
      if (c.done.size() != p->done.size()) for (uint32_t h : c.done) if (!pd.count(h)) { qr::Ev e; e.ms = ms; e.kind = "done"; e.guid = qs; sidOf(h, e.sid); ring.Push(e); n++; }
    }
  }
  return n;
}

}
