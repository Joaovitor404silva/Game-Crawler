#pragma once
#include <stdint.h>
#include <string.h>
#include <vector>
#include <functional>

namespace cnt {

inline int InnerSlot(const char* propType) { return propType && !strcmp(propType, "ArrayProperty") ? 1 : 0; }

using ReadFn =std::function<bool(uint64_t addr, void* out, size_t n)>;

struct Layout {
  uint32_t dataPtr = 0, dataNum = 8;
  uint32_t bitsInline = 16, bitsSecondary = 32, bitsNum = 40;
  uint32_t firstFree = 48, numFree = 52;
  uint32_t inlineBits = 128;
};

struct SparseInfo { int32_t slots = 0, free = 0, bits = 0; uint64_t data = 0; };

inline bool SparseElems(const ReadFn& rd, uint64_t base, const Layout& L, uint32_t stride, uint32_t maxOut, std::vector<uint64_t>& out, SparseInfo* info = nullptr) {
  uint64_t data = 0, sec = 0; int32_t slots = 0, nbits = 0, nfree = 0;
  if (!rd(base + L.dataPtr, &data, 8) || !rd(base + L.dataNum, &slots, 4)) return false;
  if (!rd(base + L.bitsSecondary, &sec, 8) || !rd(base + L.bitsNum, &nbits, 4) || !rd(base + L.numFree, &nfree, 4)) return false;
  if (info) { info->slots = slots; info->free = nfree; info->bits = nbits; info->data = data; }
  if (slots < 0 || slots > 1000000 || nbits < 0 || nbits > 8000000 || stride == 0) return false;
  if (nbits < slots) return false;
  if (slots == 0) return true;
  uint64_t bitAddr = (uint32_t)nbits <= L.inlineBits && sec == 0 ? base + L.bitsInline : sec;
  if (!bitAddr || !data) return false;
  std::vector<uint32_t> words((slots + 31) / 32);
  if (!rd(bitAddr, words.data(), words.size() * 4)) return false;
  for (int32_t i = 0; i < slots && out.size() < maxOut; i++)
    if (words[i >> 5] & (1u << (i & 31))) out.push_back(data + (uint64_t)i * stride);
  return true;
}

inline uint32_t SparseStride(uint32_t elemSize, uint32_t align) {
  if (align < 4) align = 4;
  uint32_t body = (elemSize + 3) & ~3u;
  uint32_t link = 8;
  uint32_t s = body + link;
  return (s + align - 1) & ~(align - 1);
}

}
