#include <windows.h>
#include "uescan_api.h"

static const UEApi* api;
static int reports;

UE_EXPORT int UEPluginInit(const UEApi* a) {
  api = a;
  api->log("example loaded");
  return 1;
}
UE_EXPORT void UEPluginTick() {
  static uint64_t last; if (api->ticks() - last < 600) return; last = api->ticks();
  uintptr_t pc = api->find("pc"); if (!pc) return;
  char out[512];
  if (api->call(pc, "K2_GetActorLocation", "", out, sizeof out)) { reports++; api->log("pc 0x%llx location %s", (unsigned long long)pc, out); }
}
UE_EXPORT void UEPluginShutdown() { api->log("example shutdown after %d reports", reports); }
