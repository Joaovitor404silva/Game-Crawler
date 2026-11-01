#include <windows.h>
#include "uescan_api.h"

static const int kOverride = -1;

static const UEApi* api;
typedef bool(__fastcall* IsChangingWeaponFn)(void* self);
static IsChangingWeaponFn orig;
static volatile LONG calls;

static bool __fastcall IsChangingWeaponDetour(void* self) {
  InterlockedIncrement(&calls);
  if (kOverride < 0) return orig(self);
  return kOverride != 0;
}

UE_EXPORT int UEPluginInit(const UEApi* a) {
  api = a;
  uintptr_t t = api->addr("Agent::IsChangingWeapon");
  if (!t || !api->hook(t, (void*)IsChangingWeaponDetour, (void**)&orig)) { api->log("hook failed (addr 0x%llx)", (unsigned long long)t); return 0; }
  api->log("example loaded: IsChangingWeapon override = %d", kOverride);
  return 1;
}
UE_EXPORT void UEPluginTick() {
  static uint64_t last; if (api->ticks() - last < 600) return; last = api->ticks();
  if (calls) api->log("IsChangingWeapon called %ld times (override %d)", (long)calls, kOverride);
}
UE_EXPORT void UEPluginShutdown() { api->log("example shutdown after %ld calls", (long)calls); }
