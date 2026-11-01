#include "core.h"
#include <mutex>

std::vector<std::string> g_testLog;
static std::mutex g_tlm;
static void TestLog(const char* fmt, ...) {
  char b[4096]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
  std::lock_guard<std::mutex> l(g_tlm); g_testLog.push_back(b);
}
HostApi g_testApi = { 1, TestLog, 0, 0, nullptr, 0 };

bool EngineTry(std::string& why) { why = "no reflection in tests"; return false; }
bool EngineReady() { return true; }
void EngineTick() {}
std::string EngineReport() { return "engine none"; }
const EngineBackend* ActiveEngine() { return nullptr; }
uintptr_t ResolveObj(const std::string&) { return 0; }
std::string DescribeObj(uintptr_t) { return ""; }
std::string ObjLabel(uintptr_t) { return ""; }
bool EngineHandle(const std::string&, const std::vector<std::string>&, const std::string&, Out&) { return false; }
size_t ObjSize(uintptr_t) { return 0; }
std::string PropAtOffset(uintptr_t, int) { return ""; }

bool UPropAddr(const std::string&, const std::string&, uintptr_t&, int&, std::string&, std::string&, std::string& err) { err = "no reflection in tests"; return false; }
void InitPlugins() {}
void ShutdownPlugins() {}
void TickPlugins() {}
bool HandlePlugins(const std::string&, const std::vector<std::string>&, Out&, const std::string&) { return false; }
int PluginPreCall(uintptr_t*) { return 0; }
void PluginCallFault(const uintptr_t*, int) {}
