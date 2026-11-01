#pragma once
#include <stdint.h>

namespace rty {

struct Gate {
  uint64_t next = 0, lastLog = 0;
  bool Try(uint64_t now, uint64_t gap) { if (now < next) return false; next = now + gap; return true; }
  uint64_t WaitSec(uint64_t now) const { return now >= next ? 0 : (next - now + 999) / 1000; }
  bool Loud(uint64_t now, uint64_t every) { if (lastLog && now - lastLog < every) return false; lastLog = now; return true; }
  void Reset() { next = 0; }
};

}
