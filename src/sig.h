#pragma once
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <vector>


inline bool SigText(uintptr_t base, uintptr_t* s, size_t* n) {
  if (!base) return false;
  auto dos = (IMAGE_DOS_HEADER*)base;
  auto nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
  auto sec = IMAGE_FIRST_SECTION(nt);
  for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
    if (!memcmp(sec[i].Name, ".text\0\0", 8)) { *s = base + sec[i].VirtualAddress; *n = sec[i].Misc.VirtualSize; return true; }
  return false;
}

inline uintptr_t SigFind(uintptr_t base, const char* pat, int rip = -1) {
  std::vector<int> p;
  for (const char* c = pat; *c;) {
    if (*c == ' ') { c++; continue; }
    if (*c == '?') { p.push_back(-1); while (*c == '?') c++; continue; }
    char h[3] = { c[0], c[1], 0 }; p.push_back((int)strtoul(h, nullptr, 16)); c += 2;
  }
  uintptr_t s; size_t n;
  if (p.empty() || p[0] < 0 || !SigText(base, &s, &n)) return 0;
  const uint8_t* b = (const uint8_t*)s; uintptr_t hit = 0; int hits = 0; size_t m = p.size();
  for (size_t i = 0; i + m <= n;) {
    const void* q = memchr(b + i, p[0], n - m + 1 - i); if (!q) break;
    i = (const uint8_t*)q - b; size_t k = 1;
    for (; k < m; k++) if (p[k] >= 0 && b[i + k] != (uint8_t)p[k]) break;
    if (k == m) { hit = s + i; if (++hits > 1) return 0; }
    i++;
  }
  if (hits != 1) return 0;
  return rip < 0 ? hit : hit + rip + 4 + *(const int32_t*)(hit + rip);
}
