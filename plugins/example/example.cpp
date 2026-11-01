#include <windows.h>
#include "s2dev_api.h"

static const int kOverride = -1;

static const S2Api* api;
typedef bool(__fastcall* IsChangingWeaponFn)(void* self);
static IsChangingWeaponFn orig;
static volatile LONG calls;

static bool __fastcall IsChangingWeaponDetour(void* self) {
  InterlockedIncrement(&calls);
  if (kOverride < 0) return orig(self);
  return kOverride != 0;
}

S2_EXPORT int S2PluginInit(const S2Api* a) {
  api = a;
  uintptr_t t = api->addr("Agent::IsChangingWeapon");
  if (!t || !api->hook(t, (void*)IsChangingWeaponDetour, (void**)&orig)) { api->log("hook failed (addr 0x%llx)", (unsigned long long)t); return 0; }
  api->log("example loaded: IsChangingWeapon override = %d", kOverride);
  return 1;
}
S2_EXPORT void S2PluginTick() {
  static uint64_t last; if (api->ticks() - last < 600) return; last = api->ticks();
  if (calls) api->log("IsChangingWeapon called %ld times (override %d)", (long)calls, kOverride);
}
S2_EXPORT void S2PluginShutdown() { api->log("example shutdown after %ld calls", (long)calls); }
