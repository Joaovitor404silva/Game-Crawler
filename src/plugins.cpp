#include "core.h"
#include "home.h"
#include "plug_restore.h"
#include "../plugins/s2dev_api.h"
#include "minhook/include/MinHook.h"
#include <map>
#include <mutex>
#include <algorithm>
#include <intrin.h>
#include <tlhelp32.h>
#include <psapi.h>

extern "C" void CoreHandle(const char* line, EmitFn emit, void* ctx);
bool LookupName(const std::string& n, uintptr_t& a);

static const std::string& PDirS() { static std::string p = S2Path("plugins"); return p; }
static const std::string& ODirS() { static std::string p = S2Path("bin\\plugins"); return p; }
#define PDIR (PDirS().c_str())
#define ODIR (ODirS().c_str())

struct Plugin {
  std::string name; HMODULE mod = nullptr; std::string shadow; int gen = 0;
  void (*shutdown)() = nullptr; void (*tick)() = nullptr;
  uint64_t srcStamp = 0; std::string state = "new"; uint64_t tickExc = 0;
};
struct PHook { uintptr_t target; HMODULE owner; };
static std::recursive_mutex g_pm;
static std::map<std::string, Plugin> g_plugs;
static std::vector<PHook> g_phooks;
static std::string g_restoreMsg = "core freshly loaded";
static HANDLE g_watchThread; static volatile bool g_stop; static volatile bool g_watchOn = true;

static HMODULE ModFromAddr(void* a) { HMODULE m = nullptr; GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m); return m; }
static thread_local const char* g_cur;
static std::string PlugNameOf(HMODULE m) { if (g_cur) return g_cur; std::lock_guard<std::recursive_mutex> l(g_pm); for (auto& kv : g_plugs) if (kv.second.mod == m) return kv.first; return "?"; }
static void ApiLog(const char* fmt, ...) {
  char b[4000]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
  LOG("[plug %s] %s", PlugNameOf(ModFromAddr(_ReturnAddress())).c_str(), b);
}
static uintptr_t ApiAddr(const char* e) {
  uintptr_t x, ex, im; if (LookupName(e, x)) return x;
  if (LookupNative(e, ex, im)) return im ? im : ex;
  uint64_t v; std::string err; return EvalAddr(e, v, err) ? (uintptr_t)v : 0;
}
static const char* ApiSym(uintptr_t a, char* buf, int n) { strncpy_s(buf, n, Sym(a).c_str(), _TRUNCATE); return buf; }
static int ApiHook(uintptr_t t, void* detour, void** orig) {
  HMODULE owner = ModFromAddr(detour);
  MH_STATUS s = MH_CreateHook((void*)t, detour, orig);
  if (s == MH_OK) s = MH_EnableHook((void*)t);
  LOG("[plug %s] hook %s -> %s", PlugNameOf(owner).c_str(), Sym(t).c_str(), s == MH_OK ? "ok" : MH_StatusToString(s));
  if (s != MH_OK) { MH_RemoveHook((void*)t); return 0; }
  std::lock_guard<std::recursive_mutex> l(g_pm); g_phooks.push_back({ t, owner }); return 1;
}
static int ApiUnhook(uintptr_t t) {
  std::lock_guard<std::recursive_mutex> l(g_pm);
  auto it = std::find_if(g_phooks.begin(), g_phooks.end(), [&](const PHook& h) { return h.target == t; }); if (it == g_phooks.end()) return 0;
  MH_DisableHook((void*)t); MH_RemoveHook((void*)t); g_phooks.erase(it); return 1;
}
static uintptr_t ApiFind(const char* s) { return ResolveObj(s); }
static std::string RunCmd(const std::string& line) {
  t_jsonSuppress++; std::string r; CoreHandle(line.c_str(), [](void* c, const char* t) { *(std::string*)c += t; }, &r); t_jsonSuppress--;
  while (!r.empty() && r.back() == '\n') r.pop_back(); return r;
}
static int Copy(const std::string& s, char* out, int n) { if (out && n > 0) strncpy_s(out, n, s.c_str(), _TRUNCATE); return (int)s.size(); }
static int ApiCmd(const char* line, char* out, int n) { return Copy(RunCmd(line), out, n); }
static int ApiGet(uintptr_t o, const char* path, char* out, int n) {
  std::string r = RunCmd(Fmt("get 0x%llx %s", (unsigned long long)o, path));
  size_t p = r.find(" = "); if (p == std::string::npos) { Copy(r, out, n); return -1; }
  std::string v = r.substr(p + 3); size_t nl = v.find('\n'); if (nl != std::string::npos) v = v.substr(0, nl); Copy(v, out, n); return 1;
}
static int ApiSet(uintptr_t o, const char* path, const char* val, char* out, int n) {
  std::string r = RunCmd(Fmt("set 0x%llx %s \"%s\"", (unsigned long long)o, path, val)); Copy(r, out, n); return r.rfind("set ", 0) == 0 ? 1 : 0;
}
static int ApiCall(uintptr_t o, const char* fn, const char* args, char* out, int n) {
  std::string r = RunCmd(Fmt("call 0x%llx %s %s", (unsigned long long)o, fn, args ? args : "")); Copy(r, out, n); return r.rfind("ok", 0) == 0 ? 1 : 0;
}
static uint64_t ApiTicks() { return H->tickCount; }
static S2Api g_api = { 1, 0, ApiLog, ApiAddr, ApiSym, ApiHook, ApiUnhook, ApiFind, ApiGet, ApiSet, ApiCall, ApiCmd, ApiTicks };

static uint64_t SrcStamp(const std::string& name) {
  uint64_t best = 0; WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA((std::string(PDIR) + "\\" + name + "\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return 0;
  do {
    std::string f = Lower(fd.cFileName); if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    if (f.size() < 3 || (f.substr(f.size() - 4) != ".cpp" && f.substr(f.size() - 2) != ".h")) continue;
    uint64_t t = ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime; best = std::max(best, t);
  } while (FindNextFileA(h, &fd));
  FindClose(h); return best;
}
static std::vector<std::string> ListPluginDirs() {
  std::vector<std::string> r; WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA((std::string(PDIR) + "\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return r;
  do { if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.') r.push_back(fd.cFileName); } while (FindNextFileA(h, &fd));
  FindClose(h); return r;
}
static bool SafeInitPlug(int (*f)(const S2Api*), int& rc) { __try { rc = f(&g_api); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
static void SafeVoid(void (*f)()) { __try { f(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

static int PluginThreads(HMODULE m) {
  typedef LONG(NTAPI* QIT)(HANDLE, int, PVOID, ULONG, PULONG);
  static QIT q = (QIT)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
  MODULEINFO mi{}; if (!q || !GetModuleInformation(GetCurrentProcess(), m, &mi, sizeof mi)) return 0;
  uintptr_t b = (uintptr_t)mi.lpBaseOfDll, e = b + mi.SizeOfImage; int n = 0;
  HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); THREADENTRY32 te{ sizeof te };
  for (BOOL ok = Thread32First(s, &te); ok; ok = Thread32Next(s, &te)) {
    if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
    HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID); if (!h) continue;
    uintptr_t sa = 0; if (q(h, 9  , &sa, sizeof sa, nullptr) == 0 && sa >= b && sa < e) n++;
    CloseHandle(h);
  }
  CloseHandle(s); return n;
}
static void UnloadLocked(Plugin& p) {
  if (!p.mod) return;
  g_cur = p.name.c_str(); if (p.shutdown) SafeVoid(p.shutdown); g_cur = nullptr;
  p.tick = nullptr; p.shutdown = nullptr;
  int n = 0;
  for (auto it = g_phooks.begin(); it != g_phooks.end();) {
    if (it->owner == p.mod) { MH_DisableHook((void*)it->target); MH_RemoveHook((void*)it->target); it = g_phooks.erase(it); n++; } else ++it;
  }
  Sleep(n ? 50 : 0);
  int alive = 0; for (int i = 0; i < 40 && (alive = PluginThreads(p.mod)) > 0; i++) Sleep(50);
  if (alive) { LOG("[plug %s] NOT freed: %d thread(s) started by it are still running (stop them in S2PluginShutdown); module leaked", p.name.c_str(), alive); p.mod = nullptr; return; }
  FreeLibrary(p.mod); p.mod = nullptr; DeleteFileA(p.shadow.c_str());
  LOG("[plug %s] unloaded (%d hooks removed)", p.name.c_str(), n);
}
static std::string LoadLocked(Plugin& p) {
  UnloadLocked(p);
  std::string dll = std::string(ODIR) + "\\" + p.name + ".dll";
  if (GetFileAttributesA(dll.c_str()) == INVALID_FILE_ATTRIBUTES) { p.state = "not built"; return p.name + ": not built"; }
  std::string sd = std::string(ODIR) + "\\shadow"; CreateDirectoryA(sd.c_str(), nullptr);
  p.shadow = Fmt("%s\\%s_%lu_%d.dll", sd.c_str(), p.name.c_str(), GetCurrentProcessId(), ++p.gen);
  if (!CopyFileA(dll.c_str(), p.shadow.c_str(), FALSE)) { p.state = "copy failed"; return p.name + ": copy failed"; }
  p.mod = LoadLibraryA(p.shadow.c_str());
  if (!p.mod) { p.state = Fmt("LoadLibrary failed %lu", GetLastError()); return p.name + ": " + p.state; }
  auto init = (int (*)(const S2Api*))GetProcAddress(p.mod, "S2PluginInit");
  p.shutdown = (void (*)())GetProcAddress(p.mod, "S2PluginShutdown");
  auto tick = (void (*)())GetProcAddress(p.mod, "S2PluginTick");
  int rc = 0;
  g_cur = p.name.c_str(); bool iok = init && SafeInitPlug(init, rc); g_cur = nullptr;
  if (!iok || rc != 1) { p.state = init ? Fmt("init failed rc=%d", rc) : "no S2PluginInit"; UnloadLocked(p); return p.name + ": " + p.state; }
  p.tick = tick; p.state = Fmt("loaded gen %d", p.gen); p.tickExc = 0;
  LOG("[plug %s] loaded gen %d", p.name.c_str(), p.gen);
  return p.name + ": " + p.state;
}
static std::string Build(const std::string& name, bool& ok) {
  std::string logf = std::string(ODIR) + "\\" + name + ".build.log"; CreateDirectoryA(ODIR, nullptr);
  std::string cmd = Fmt("cmd.exe /c \"\"%s\\build_plugin.bat\" %s > \"%s\" 2>&1\"", PDIR, name.c_str(), logf.c_str());
  STARTUPINFOA si{ sizeof(si) }; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE; PROCESS_INFORMATION pi{};
  std::vector<char> cl(cmd.begin(), cmd.end()); cl.push_back(0);
  DWORD t0 = GetTickCount();
  if (!CreateProcessA(nullptr, cl.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &si, &pi)) { ok = false; return "CreateProcess failed"; }
  WaitForSingleObject(pi.hProcess, 120000); DWORD ec = 1; GetExitCodeProcess(pi.hProcess, &ec); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
  ok = ec == 0;
  std::string r = Fmt("%s: build %s in %lu ms (log %s)", name.c_str(), ok ? "OK" : "FAILED", GetTickCount() - t0, logf.c_str());
  LOG("[plug %s] %s", name.c_str(), r.c_str());
  return r;
}
static std::string Reload(const std::string& name) {
  std::string r, err;
  bool ran = RunOnGame([&]() { std::lock_guard<std::recursive_mutex> l(g_pm); Plugin& p = g_plugs[name]; p.name = name; r = LoadLocked(p); }, 10000, &err);
  return ran ? r : name + ": " + err;
}
static std::string BuildAndReload(const std::string& name) {
  { std::lock_guard<std::recursive_mutex> l(g_pm); Plugin& p = g_plugs[name]; p.name = name; p.srcStamp = SrcStamp(name); }
  bool ok; std::string r = Build(name, ok);
  if (!ok) { std::lock_guard<std::recursive_mutex> l(g_pm); g_plugs[name].state = "build failed (old copy kept)"; return r; }
  return r + "\n" + Reload(name);
}

static DWORD WINAPI WatchThread(LPVOID) {
  while (!g_stop) {
    for (int i = 0; i < 10 && !g_stop; i++) Sleep(100);
    if (g_stop || !g_watchOn) continue;
    for (auto& n : ListPluginDirs()) {
      uint64_t st = SrcStamp(n); if (!st) continue;
      uint64_t old; { std::lock_guard<std::recursive_mutex> l(g_pm); auto it = g_plugs.find(n); old = it == g_plugs.end() ? 0 : it->second.srcStamp; }
      if (st != old && !g_stop) { LOG("[plug %s] source changed -> rebuild", n.c_str()); BuildAndReload(n); }
    }
  }
  return 0;
}

void InitPlugins() {
  g_api.base = H->base; g_stop = false;
  char rs[1024] = {}; GetEnvironmentVariableA("ANOMALY_PLUG_RESTORE", rs, sizeof rs);
  std::vector<std::string> want = plr::Split(rs);
  char np[8]; bool noplug = GetEnvironmentVariableA("ANOMALY_NOPLUG", np, sizeof np) && np[0] == '1';
  if (noplug) g_watchOn = false;
  else {
    for (auto& n : ListPluginDirs()) {
      std::lock_guard<std::recursive_mutex> l(g_pm); Plugin& p = g_plugs[n]; p.name = n; p.srcStamp = SrcStamp(n);
      std::string dll = std::string(ODIR) + "\\" + n + ".dll";
      WIN32_FILE_ATTRIBUTE_DATA a; bool fresh = GetFileAttributesExA(dll.c_str(), GetFileExInfoStandard, &a) &&
        (((uint64_t)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime) >= p.srcStamp;
      if (!fresh) p.srcStamp = 0;
      else LoadLocked(p);
    }
  }
  std::vector<std::string> back, lost;
  { std::lock_guard<std::recursive_mutex> l(g_pm);
    for (auto& n : want) {
      Plugin& p = g_plugs[n]; p.name = n;
      if (!p.mod) { if (p.srcStamp == 0) p.srcStamp = SrcStamp(n); LoadLocked(p); }
      (p.mod ? back : lost).push_back(n);
    }
    g_restoreMsg = want.empty() ? "no plugins were loaded before the core reload" : "plugins back after core reload: " + (back.empty() ? std::string("none") : plr::Join(back)) + (lost.empty() ? "" : "; FAILED: " + plr::Join(lost));
  }
  LOG("[plug] %s", g_restoreMsg.c_str());
  if (!noplug) g_watchThread = CreateThread(nullptr, 0, WatchThread, nullptr, 0, nullptr);
}
void ShutdownPlugins() {
  g_stop = true;
  if (g_watchThread) { WaitForSingleObject(g_watchThread, 130000); CloseHandle(g_watchThread); g_watchThread = nullptr; }
  std::lock_guard<std::recursive_mutex> l(g_pm);
  std::vector<std::string> was;
  for (auto& kv : g_plugs) if (kv.second.mod) was.push_back(kv.first);
  SetEnvironmentVariableA("ANOMALY_PLUG_RESTORE", plr::Join(was).c_str());
  for (auto& kv : g_plugs) UnloadLocked(kv.second);
  g_plugs.clear();
}
static bool SafeTick(void (*f)()) { __try { f(); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
void TickPlugins() {
  std::lock_guard<std::recursive_mutex> l(g_pm);
  for (auto& kv : g_plugs) if (kv.second.tick) { g_cur = kv.first.c_str(); if (!SafeTick(kv.second.tick)) kv.second.tickExc++; g_cur = nullptr; }
}
static std::string ReadFileStr(const std::string& f, size_t maxTail) {
  FILE* fp; if (fopen_s(&fp, f.c_str(), "rb")) return "(no " + f + ")";
  std::string d; char b[4096]; size_t k; while ((k = fread(b, 1, sizeof(b), fp)) > 0) d.append(b, k); fclose(fp);
  return d.size() > maxTail ? d.substr(d.size() - maxTail) : d;
}
bool HandlePlugins(const std::string& c, const std::vector<std::string>& t, Out& o) {
  if (c != "plug") return false;
  std::string sub = t.size() > 1 ? Lower(t[1]) : "list", n = t.size() > 2 ? t[2] : "";
  if (sub == "list") {
    std::lock_guard<std::recursive_mutex> l(g_pm);
    for (auto& d : ListPluginDirs()) {
      auto it = g_plugs.find(d); int hk = 0;
      if (it != g_plugs.end()) for (auto& h : g_phooks) hk += h.owner == it->second.mod && it->second.mod;
      o.f("%-20s %s hooks=%d tickExc=%llu", d.c_str(), it == g_plugs.end() ? "unknown" : it->second.state.c_str(), hk, it == g_plugs.end() ? 0ull : it->second.tickExc);
    }
    o.f("watch %s (folder %s)", g_watchOn ? "on" : "off", PDIR);
    o.line(g_restoreMsg); return true;
  }
  if (sub == "watch") { g_watchOn = n != "off"; o.f("watch %s", g_watchOn ? "on" : "off"); return true; }
  if (n.empty()) { o.line("usage: plug list | plug build <name> | plug reload <name> | plug unload <name> | plug log <name> | plug watch on|off"); return true; }
  if (sub == "build") { o.line(BuildAndReload(n)); return true; }
  if (sub == "reload") { o.line(Reload(n)); return true; }
  if (sub == "unload") {
    std::string err; bool ran = RunOnGame([&]() { std::lock_guard<std::recursive_mutex> l(g_pm); auto it = g_plugs.find(n); if (it != g_plugs.end()) { UnloadLocked(it->second); it->second.state = "unloaded"; } }, 10000, &err);
    o.line(ran ? n + ": unloaded" : err); return true;
  }
  if (sub == "log") { o.line(ReadFileStr(std::string(ODIR) + "\\" + n + ".build.log", 6000)); return true; }
  o.line("unknown plug subcommand"); return true;
}
