#pragma once
#include <windows.h>
#include <stdint.h>

inline int WalkStack(CONTEXT ctx, uint64_t* out, int maxn) {
  int n = 0;
  uint64_t startRsp = ctx.Rsp;
  __try {
    while (n < maxn) {
      out[n++] = ctx.Rip;
      DWORD64 ib = 0;
      PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(ctx.Rip, &ib, nullptr);
      uint64_t prevRsp = ctx.Rsp;
      if (!rf) {
        if (ctx.Rsp < 0x10000) break;
        ctx.Rip = *(uint64_t*)ctx.Rsp; ctx.Rsp += 8;
      } else {
        PVOID hd = nullptr; DWORD64 ef = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, ctx.Rip, rf, &ctx, &hd, &ef, nullptr);
      }
      if (ctx.Rip < 0x10000 || ctx.Rsp <= prevRsp || ctx.Rsp - startRsp > (64u << 20)) break;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
  return n;
}

inline int WalkFromEntry(uint64_t fnEntry, const uint64_t* retAddrSlot, uint64_t* out, int maxn) {
  CONTEXT c; memset(&c, 0, sizeof c);
  c.Rip = fnEntry; c.Rsp = (uint64_t)retAddrSlot; c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
  return WalkStack(c, out, maxn);
}
