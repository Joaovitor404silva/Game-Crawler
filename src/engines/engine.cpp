#include "core.h"
#include <mutex>

extern const EngineBackend kUnreal;
static const EngineBackend* const kEngines[] = { &kUnreal };

static const EngineBackend* volatile g_active;
static ULONGLONG g_nextProbe;
static std::string g_report = "not probed yet";
static std::mutex g_em;

bool ImageHasBytes(const void* p, size_t n) {
  auto nt = (IMAGE_NT_HEADERS*)(H->base + ((IMAGE_DOS_HEADER*)H->base)->e_lfanew);
  auto sec = IMAGE_FIRST_SECTION(nt); const uint8_t* pat = (const uint8_t*)p;
  for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
    DWORD ch = sec[i].Characteristics;
    if (!(ch & IMAGE_SCN_MEM_READ) || (ch & IMAGE_SCN_MEM_EXECUTE)) continue;
    const uint8_t* b = (const uint8_t*)(H->base + sec[i].VirtualAddress);
    size_t len = std::min<size_t>(sec[i].Misc.VirtualSize, sec[i].SizeOfRawData ? sec[i].SizeOfRawData : sec[i].Misc.VirtualSize);
    for (size_t k = 0; k + n <= len; k++) {
      const void* hit = memchr(b + k, pat[0], len - n + 1 - k); if (!hit) break;
      k = (const uint8_t*)hit - b; if (!memcmp(b + k, pat, n)) return true;
    }
  }
  return false;
}
bool ImageHasWide(const wchar_t* s) { return ImageHasBytes(s, wcslen(s) * sizeof(wchar_t)); }
bool ExeNameHas(const char* s) {
  char p[MAX_PATH]; DWORD n = GetModuleFileNameA(nullptr, p, MAX_PATH); if (!n || n >= MAX_PATH) return false;
  return strstr(p, s) != nullptr;
}

const EngineBackend* ActiveEngine() {
  if (g_active) return g_active;
  std::lock_guard<std::mutex> l(g_em);
  ULONGLONG now = GetTickCount64(); if (g_active || now < g_nextProbe) return g_active;
  g_nextProbe = now + 5000; std::string r;
  for (auto e : kEngines) {
    bool d = e->detect(); r += std::string(r.empty() ? "" : ", ") + e->name + (d ? " yes" : " no");
    if (d && !g_active) { g_active = e; LOG("engine backend: %s", e->name); }
  }
  g_report = r;
  return g_active;
}
std::string EngineReport() {
  auto e = ActiveEngine(); std::lock_guard<std::mutex> l(g_em);
  std::string s = std::string("engine ") + (e ? e->name : "none (generic commands only)") + "; probed: " + g_report;
  if (e) { s += e->ready() ? "; backend ready" : "; backend not ready yet"; }
  return s;
}

bool EngineTry(std::string& why) { auto e = ActiveEngine(); if (!e) { why = "no engine backend detected"; return false; } return e->tryInit(why); }
bool EngineReady() { auto e = ActiveEngine(); return e && e->ready(); }
void EngineTick() { auto e = g_active; if (e) e->tick(); }
bool EngineHandle(const std::string& c, const std::vector<std::string>& t, const std::string& line, Out& o) { auto e = ActiveEngine(); return e && e->handle(c, t, line, o); }
uintptr_t ResolveObj(const std::string& s) { auto e = ActiveEngine(); return e ? e->resolveObj(s) : 0; }
size_t ObjSize(uintptr_t a) { auto e = g_active; return e ? e->objSize(a) : 0; }
std::string PropAtOffset(uintptr_t obj, int off) { auto e = g_active; return e ? e->fieldAt(obj, off) : ""; }
std::string DescribeObj(uintptr_t a) { auto e = g_active; return e ? e->describe(a) : ""; }
std::string ObjLabel(uintptr_t a) { auto e = g_active; return e ? e->label(a) : ""; }
bool UPropAddr(const std::string& obj, const std::string& path, uintptr_t& addr, int& size, std::string& type, std::string& val, std::string& err) {
  auto e = ActiveEngine(); if (!e) { err = "no engine backend detected"; return false; }
  return e->propAddr(obj, path, addr, size, type, val, err);
}
