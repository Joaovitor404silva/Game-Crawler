#pragma once
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#define SIG_TICK "41 57 41 56 41 55 41 54 56 57 55 53 48 81 EC E8 00 00 00 0F 29 BC 24 D0 00 00 00 0F 29 B4 24 C0 00 00 00 44 89 C3 0F 28 F1"
#define SIG_FNAME_TOSTRING "41 57 41 56 41 54 56 57 55 53 48 81 EC 30 08 00 00 48 89 D6 48 89 CF 48 8B 05 ?? ?? ?? ?? 48 31 E0 48 89 84 24 28 08 00 00 83 79 04 00"
#define SIG_FNAME_CTOR "41 57 41 56 56 57 53 48 81 EC 40 04 00 00 48 89 CE 48 8B 05 ?? ?? ?? ?? 48 31 E0 48 89 84 24 38 04 00 00 B1 01"
#define SIG_GUOBJECTARRAY "03 0D ?? ?? ?? ?? 8B 15 ?? ?? ?? ??"
#define SIG_GUOBJECTARRAY_RIP 2
#define SIG_GCONSOLEMANAGER "48 8B 0D ?? ?? ?? ?? EB 9E 48 8B 10"
#define SIG_GCONSOLEMANAGER_RIP 3
#define SIG_SM "48 31 E0 48 89 84 24 80 00 00 00 48 8B 1D ?? ?? ?? ?? BE FF FF FF FF 48 85 DB 74 78 48 89 CF 48 8B 05"
#define SIG_SM_RIP 14
#define SIG_NAMEPOOL "41 8B 55 00 48 8D 35 ?? ?? ?? ?? 48 89 F1 E8 ?? ?? ?? ?? 31 DB 41 89 5D 00 8B 17 48 89 F1 E8"
#define SIG_NAMEPOOL_RIP 7

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
