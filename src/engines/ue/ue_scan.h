#pragma once
#include <stdint.h>
#include <string.h>
#include <functional>
#include <string>
#include <vector>
#include <unordered_map>

namespace ues {
using ReadFn = std::function<bool(uint64_t addr, void* out, size_t n)>;

inline bool Plausible(uint64_t p) { return p >= 0x10000 && p < 0x7FFFFFFFFFFFull; }

struct ObjArrayHit { uint64_t va = 0; int num = 0, max = 0, stride = 0; };

inline int ProbeStride(const ReadFn& rd, uint64_t chunks) {
  uint64_t c0 = 0; if (!rd(chunks, &c0, 8) || !Plausible(c0)) return 0;
  for (int s : { 24, 16, 32 }) {
    bool ok = true;
    for (int i = 1; i <= 3 && ok; i++) {
      uint64_t o = 0; int idx = -1;
      ok = rd(c0 + (uint64_t)i * s, &o, 8) && Plausible(o) && !(o & 7) && rd(o + 0xC, &idx, 4) && idx == i;
    }
    if (ok) return s;
  }
  return 0;
}

inline ObjArrayHit FindObjArray(const uint8_t* data, size_t n, uint64_t va, const ReadFn& rd) {
  ObjArrayHit best;
  for (size_t o = 0; o + 0x30 <= n; o += 8) {
    int mx, num, mc, nc; memcpy(&mx, data + o + 0x20, 4); memcpy(&num, data + o + 0x24, 4); memcpy(&mc, data + o + 0x28, 4); memcpy(&nc, data + o + 0x2C, 4);
    if (mx <= 0 || mx > 0x8000000 || num <= 0 || num > mx) continue;
    if (mc != (mx + 65535) / 65536 || nc < (num + 65535) / 65536 || nc > mc) continue;
    uint64_t chunks; memcpy(&chunks, data + o + 0x10, 8); if (!Plausible(chunks)) continue;
    int s = ProbeStride(rd, chunks); if (!s) continue;
    if (num > best.num) { best.va = va + o; best.num = num; best.max = mx; best.stride = s; }
  }
  return best;
}

inline bool ReadEntry(const ReadFn& rd, uint64_t at, std::string& s, int* bytes = nullptr) {
  uint16_t h = 0; if (!rd(at, &h, 2)) return false;
  int len = h >> 6; bool wide = h & 1; if (len <= 0 || len > 1024) return false;
  if (wide) {
    std::vector<uint16_t> w(len); if (!rd(at + 2, w.data(), (size_t)len * 2)) return false;
    s.clear(); for (uint16_t c : w) s += c < 0x80 ? (char)c : '?';
  } else { s.assign(len, 0); if (!rd(at + 2, &s[0], len)) return false; }
  if (bytes) *bytes = (2 + len * (wide ? 2 : 1) + 1) & ~1;
  return true;
}

inline bool IsPoolBlock0(const ReadFn& rd, uint64_t b) {
  std::string a, c; int n = 0;
  return ReadEntry(rd, b, a, &n) && a == "None" && ReadEntry(rd, b + n, c) && c == "ByteProperty";
}

inline bool ChainOk(const ReadFn& rd, uint64_t at, int count) {
  for (int i = 0; i < count; i++) {
    std::string s; int bytes = 0; if (!ReadEntry(rd, at, s, &bytes)) return false;
    for (char c : s) if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7e) return false;
    at += bytes;
  }
  return true;
}

inline int BlocksValid(const uint8_t* data, size_t n, size_t o, const ReadFn& rd) {
  int k = 1;
  for (; k < 16 && o + k * 8 + 8 <= n; k++) {
    uint64_t b; memcpy(&b, data + o + k * 8, 8);
    if (!b) break;
    if (!Plausible(b) || !ChainOk(rd, b, 20)) return 0;
  }
  return k;
}

inline uint64_t FindNameBlocks(const uint8_t* data, size_t n, uint64_t va, const ReadFn& rd) {
  uint64_t best = 0; int bestK = 0;
  for (size_t o = 0x10; o + 16 <= n; o += 8) {
    uint64_t b0; memcpy(&b0, data + o, 8); if (!Plausible(b0)) continue;
    uint16_t h; if (!rd(b0, &h, 2) || (h >> 6) != 4 || (h & 1)) continue;
    if (!IsPoolBlock0(rd, b0)) continue;
    uint32_t cur; memcpy(&cur, data + o - 8, 4); if (cur > 8191) continue;
    int k = BlocksValid(data, n, o, rd);
    if (k > bestK) { bestK = k; best = va + o; }
  }
  return best;
}

inline bool NameAt(const ReadFn& rd, uint64_t blocks, uint32_t idx, std::string& s) {
  uint32_t blk = idx >> 16, off = (idx & 0xFFFF) * 2; if (blk > 8191) return false;
  uint64_t b = 0; if (!rd(blocks + (uint64_t)blk * 8, &b, 8) || !Plausible(b)) return false;
  return ReadEntry(rd, b + off, s);
}

inline void LowerIn(std::string& s) { for (auto& c : s) if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a'; }

inline void IndexPool(const ReadFn& rd, uint64_t blocks, std::unordered_map<std::string, uint32_t>& out) {
  out.clear();
  for (uint32_t blk = 0; blk < 8192; blk++) {
    uint64_t b = 0; if (!rd(blocks + (uint64_t)blk * 8, &b, 8) || !Plausible(b)) break;
    for (uint32_t off = 0; off + 2 <= 0x20000;) {
      std::string s; int n = 0; if (!ReadEntry(rd, b + off, s, &n)) break;
      LowerIn(s); out.emplace(s, (blk << 16) | (off / 2)); off += n;
    }
  }
}

inline bool SplitNumber(const std::string& s, std::string& base, uint32_t& number) {
  size_t u = s.rfind('_'); if (u == std::string::npos || u == 0 || u + 1 >= s.size()) return false;
  std::string d = s.substr(u + 1); for (char c : d) if (c < '0' || c > '9') return false;
  if (d.size() > 1 && d[0] == '0') return false;
  if (d.size() > 9) return false;
  base = s.substr(0, u); number = (uint32_t)strtoul(d.c_str(), nullptr, 10) + 1; return true;
}

inline bool LookupName(const std::unordered_map<std::string, uint32_t>& pool, const std::string& s, uint64_t& out) {
  std::string k = s; LowerIn(k);
  auto it = pool.find(k); if (it != pool.end()) { out = it->second; return true; }
  std::string base; uint32_t num;
  if (SplitNumber(k, base, num)) { it = pool.find(base); if (it != pool.end()) { out = (uint64_t)it->second | ((uint64_t)num << 32); return true; } }
  return false;
}

struct Insn { unsigned len; uint8_t op; uint8_t modrm; uint32_t imm; bool mem; int32_t disp; };
using DecodeFn = std::function<bool(uint64_t at, Insn& out)>;

inline bool TestsNativeFlag(const DecodeFn& dec, uint64_t fn, int flagsOff, int maxBytes = 0x400) {
  for (uint64_t p = fn; p < fn + maxBytes;) {
    Insn i{}; if (!dec(p, i) || !i.len) return false;
    bool r0 = i.mem && ((i.modrm >> 3) & 7) == 0;
    if (r0 && i.op == 0xF7 && i.imm == 0x400 && i.disp == flagsOff) return true;
    if (r0 && i.op == 0xF6 && (i.imm & 0xFF) == 4 && i.disp == flagsOff + 1) return true;
    if (i.op == 0xCC) return false;
    p += i.len;
  }
  return false;
}

inline int FindProcessEventSlot(const ReadFn& rd, const DecodeFn& dec, uint64_t vtable, uint64_t textBeg, uint64_t textEnd, int flagsOff, std::vector<int>* cands = nullptr) {
  int first = -1;
  for (int i = 0; i < 0x90; i++) {
    uint64_t f = 0; if (!rd(vtable + (uint64_t)i * 8, &f, 8) || f < textBeg || f >= textEnd) break;
    if (TestsNativeFlag(dec, f, flagsOff)) { if (cands) cands->push_back(i); if (first < 0) first = i; }
  }
  return first;
}
}
