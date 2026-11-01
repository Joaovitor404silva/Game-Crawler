#include "../src/core.h"
#include <mutex>

std::vector<std::string> g_testLog;
static std::mutex g_tlm;
static void TestLog(const char* fmt, ...) {
  char b[4096]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
  std::lock_guard<std::mutex> l(g_tlm); g_testLog.push_back(b);
}
HostApi g_testApi = { 1, TestLog, 0, 0, nullptr, 0 };

void InitUObj() {}
void TickWatches() {}
uintptr_t ResolveObj(const std::string&) { return 0; }
std::string DescribeObj(uintptr_t) { return ""; }
bool HandleUObj(const std::string&, const std::vector<std::string>&, const std::string&, Out&) { return false; }
size_t ObjSize(uintptr_t) { return 0; }
std::string PropAtOffset(uintptr_t, int) { return ""; }
std::string ObjName(uintptr_t) { return "?"; }
bool IsObj(uintptr_t) { return false; }
bool UPropAddr(const std::string&, const std::string&, uintptr_t&, int&, std::string&, std::string&, std::string& err) { err = "no reflection in tests"; return false; }
void InitPlugins() {}
void ShutdownPlugins() {}
void TickPlugins() {}
bool HandlePlugins(const std::string&, const std::vector<std::string>&, Out&) { return false; }
