#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct UEApi {
  int version;
  uintptr_t base;
  void (*log)(const char* fmt, ...);
  uintptr_t (*addr)(const char* expr);
  const char* (*sym)(uintptr_t a, char* buf, int n);
  int (*hook)(uintptr_t target, void* detour, void** orig);
  int (*unhook)(uintptr_t target);
  uintptr_t (*find)(const char* obj);
  int (*get)(uintptr_t obj, const char* path, char* out, int n);
  int (*set)(uintptr_t obj, const char* path, const char* value, char* out, int n);
  int (*call)(uintptr_t obj, const char* func, const char* args, char* out, int n);
  int (*cmd)(const char* line, char* out, int n);
  uint64_t (*ticks)(void);
  int (*read)(uintptr_t addr, void* out, size_t n);
  uintptr_t (*sig)(const char* pattern, int rip);
  const char* (*path)(const char* rel, char* buf, int n);
  const char* (*describe)(uintptr_t obj, char* buf, int n);
} UEApi;
#ifdef __cplusplus
}
#define UE_EXPORT extern "C" __declspec(dllexport)
#else
#define UE_EXPORT __declspec(dllexport)
#endif
