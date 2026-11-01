#pragma once
#include <stdint.h>

namespace hwh {

struct Slot { bool used = false; int hookId = 0; };

inline unsigned Pick(const Slot* s, int n, int id) {
  unsigned m = 0;
  for (int i = 0; i < n; i++) if (s[i].used && s[i].hookId != 0 && (id < 0 || s[i].hookId == id)) m |= 1u << i;
  return m;
}

inline int Count(unsigned m) { int c = 0; for (; m; m &= m - 1) c++; return c; }

}
