#pragma once
#include <windows.h>
#include <stdint.h>
typedef void (*EmitFn)(void* ctx, const char* text);
struct HostApi {
  int version;
  void (*log)(const char* fmt, ...);
  uintptr_t base;
  uint64_t tickCount;
  void* engineThis;
  DWORD gameThreadId;
};
typedef int  (*CoreInitFn)(HostApi*);
typedef void (*CoreShutdownFn)();
typedef void (*CoreHandleFn)(const char* line, EmitFn emit, void* ctx);
typedef void (*CoreTickFn)();
