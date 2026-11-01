
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string>
#include "api.h"
#include "home.h"
#include "minhook/include/MinHook.h"
#include <tlhelp32.h>
#pragma comment(lib, "ws2_32.lib")

static const std::string& LogPathS() { static std::string p = UEnv("UESCAN_LOGDIR", UEHome()) + "\\uescan.log"; return p; }
#define LOG_PATH (LogPathS().c_str())
static std::string CoreSrc() { char e[MAX_PATH]; DWORD n = GetEnvironmentVariableA("UESCAN_CORE", e, sizeof e); return n && n < sizeof e ? std::string(e) : UEPath("bin\\UEScannerCore.dll"); }
static const std::string& ShadowDirS() { static std::string p = UEPath("bin\\shadow"); return p; }
#define SHADOW_DIR (ShadowDirS().c_str())
static int PORT = 27060;

static CRITICAL_SECTION g_logCs, g_cmdCs;
static HostApi g_api;
static SRWLOCK g_coreLock = SRWLOCK_INIT;
static HMODULE g_core;
static CoreShutdownFn g_coreShutdown;
static CoreHandleFn g_coreHandle;
static CoreTickFn g_coreTick;
static int g_coreGen;
typedef void(__fastcall* PEFn)(void*, void*, void*);
static PEFn g_origPE;
static void* g_peTarget;
static DWORD g_mainTid;
static LONGLONG g_lastPump, g_pumpGap;
static bool g_inPump;

static void Log(const char* fmt, ...) {
  char buf[4096];
  SYSTEMTIME t; GetLocalTime(&t);
  int n = sprintf_s(buf, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
  static char tag[16] = "?"; if (tag[0] == '?') { tag[0] = 0; char ps[16]; if (GetEnvironmentVariableA("UESCAN_PORT", ps, sizeof ps)) sprintf_s(tag, "{%s} ", ps); }
  n += sprintf_s(buf + n, sizeof(buf) - n, "%s", tag);
  va_list ap; va_start(ap, fmt); vsnprintf(buf + n, sizeof(buf) - n - 2, fmt, ap); va_end(ap);
  size_t len = strlen(buf); if (len == 0 || buf[len - 1] != '\n') { buf[len++] = '\n'; buf[len] = 0; }
  EnterCriticalSection(&g_logCs);
  FILE* f; if (fopen_s(&f, LOG_PATH, "ab") == 0) { fwrite(buf, 1, len, f); fclose(f); }
  LeaveCriticalSection(&g_logCs);
}

extern "C" __declspec(dllexport) int UEScannerInit() { return 1; }

static int SafeInit(CoreInitFn f, HostApi* a) { __try { return f(a); } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; } }
static bool SafeHandle(CoreHandleFn f, const char* l, EmitFn e, void* c) { __try { f(l, e, c); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
static void SafeShutdown(CoreShutdownFn f) { __try { f(); } __except (EXCEPTION_EXECUTE_HANDLER) { Log("core shutdown exception"); } }

static void Pump() {
  g_api.tickCount++;
  if (TryAcquireSRWLockShared(&g_coreLock)) {
    if (g_coreTick) { __try { g_coreTick(); } __except (EXCEPTION_EXECUTE_HANDLER) { Log("core tick exception %08x", GetExceptionCode()); } }
    ReleaseSRWLockShared(&g_coreLock);
  }
}
static void __fastcall HkPE(void* obj, void* fn, void* parms) {
  if (!g_inPump && GetCurrentThreadId() == g_mainTid) {
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (now.QuadPart - g_lastPump >= g_pumpGap) { g_lastPump = now.QuadPart; g_inPump = true; Pump(); g_inPump = false; }
  }
  g_origPE(obj, fn, parms);
}
static int HookPump(void* target) {
  if (g_peTarget) return g_peTarget == target ? 1 : -2;
  MH_STATUS a = MH_CreateHook(target, (void*)HkPE, (void**)&g_origPE), b = a == MH_OK ? MH_EnableHook(target) : a;
  Log("ProcessEvent pump at %p: create=%d enable=%d main thread %lu", target, a, b, g_mainTid);
  if (b != MH_OK) return -1;
  g_peTarget = target; return 1;
}
static DWORD MainThreadId() {
  DWORD pid = GetCurrentProcessId(), best = 0; ULONGLONG bt = ~0ull;
  HANDLE sn = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); if (sn == INVALID_HANDLE_VALUE) return 0;
  THREADENTRY32 te{ sizeof te };
  for (BOOL ok = Thread32First(sn, &te); ok; ok = Thread32Next(sn, &te)) {
    if (te.th32OwnerProcessID != pid) continue;
    HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID); if (!h) continue;
    FILETIME c, e, k, u; if (GetThreadTimes(h, &c, &e, &k, &u)) { ULONGLONG t = ((ULONGLONG)c.dwHighDateTime << 32) | c.dwLowDateTime; if (t && t < bt) { bt = t; best = te.th32ThreadID; } }
    CloseHandle(h);
  }
  CloseHandle(sn); return best;
}

static void UnloadCoreLocked() {
  if (!g_core) return;
  if (g_coreShutdown) SafeShutdown(g_coreShutdown);
  g_coreTick = nullptr; g_coreHandle = nullptr; g_coreShutdown = nullptr;
  Sleep(50);
  FreeLibrary(g_core); g_core = nullptr;
}

static std::string LoadCore() {
  AcquireSRWLockExclusive(&g_coreLock);
  UnloadCoreLocked();
  CreateDirectoryA(SHADOW_DIR, nullptr);
  char shadow[MAX_PATH]; sprintf_s(shadow, "%s\\UEScannerCore_%lu_%d.dll", SHADOW_DIR, GetCurrentProcessId(), ++g_coreGen);
  std::string r;
  if (!CopyFileA(CoreSrc().c_str(), shadow, FALSE)) { r = "copy failed " + std::to_string(GetLastError()); }
  else if (!(g_core = LoadLibraryA(shadow))) { r = "LoadLibrary failed " + std::to_string(GetLastError()); }
  else {
    auto init = (CoreInitFn)GetProcAddress(g_core, "CoreInit");
    g_coreShutdown = (CoreShutdownFn)GetProcAddress(g_core, "CoreShutdown");
    g_coreHandle = (CoreHandleFn)GetProcAddress(g_core, "CoreHandle");
    auto tick = (CoreTickFn)GetProcAddress(g_core, "CoreTick");
    int ok = 0;
    ok = init ? SafeInit(init, &g_api) : 0;
    if (ok != 1) { r = "CoreInit failed " + std::to_string(ok); g_coreTick = nullptr; FreeLibrary(g_core); g_core = nullptr; g_coreHandle = nullptr; g_coreShutdown = nullptr; }
    else { g_coreTick = tick; r = std::string("core loaded gen ") + std::to_string(g_coreGen) + " " + shadow; }
  }
  ReleaseSRWLockExclusive(&g_coreLock);
  Log("%s", r.c_str());
  return r;
}

struct Conn { SOCKET s; };
static void EmitSock(void* ctx, const char* text) {
  SOCKET s = ((Conn*)ctx)->s; size_t n = strlen(text); const char* p = text;
  while (n) { int k = send(s, p, (int)n, 0); if (k <= 0) return; p += k; n -= k; }
}
static void EmitLine(Conn* c, const std::string& s) { EmitSock(c, (s + "\n").c_str()); }

static void HandleLine(Conn* c, std::string line) {
  while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
  if (line.empty()) { EmitLine(c, "."); return; }
  if (line == "ping") { char b[200]; sprintf_s(b, "pong pid=%lu base=0x%llx ticks=%llu core=%s gen=%d", GetCurrentProcessId(), (unsigned long long)g_api.base, (unsigned long long)g_api.tickCount, g_core ? "loaded" : "none", g_coreGen); EmitLine(c, b); }
  else if (line == "reload") EmitLine(c, LoadCore());
  else if (line == "unload") { AcquireSRWLockExclusive(&g_coreLock); UnloadCoreLocked(); ReleaseSRWLockExclusive(&g_coreLock); EmitLine(c, "core unloaded"); }
  else if (g_coreHandle) {
    if (!SafeHandle(g_coreHandle, line.c_str(), EmitSock, c)) EmitSock(c, "core handler exception\n");
  } else EmitLine(c, "no core loaded (try reload)");
  EmitLine(c, ".");
}

static DWORD WINAPI ClientThread(LPVOID p) {
  Conn c{ (SOCKET)(uintptr_t)p }; std::string buf; char tmp[4096];
  for (;;) {
    int k = recv(c.s, tmp, sizeof(tmp), 0); if (k <= 0) break;
    buf.append(tmp, k); size_t nl;
    while ((nl = buf.find('\n')) != std::string::npos) { std::string line = buf.substr(0, nl); buf.erase(0, nl + 1); EnterCriticalSection(&g_cmdCs); HandleLine(&c, line); LeaveCriticalSection(&g_cmdCs); }
  }
  closesocket(c.s); return 0;
}

static DWORD WINAPI MainThread(LPVOID) {
  Log("UE Scanner loader start pid=%lu base=0x%llx", GetCurrentProcessId(), (unsigned long long)g_api.base);
  Log("MinHook init %d", MH_Initialize());
  g_mainTid = MainThreadId(); g_api.gameThreadId = g_mainTid;
  LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_pumpGap = f.QuadPart / 500;
  LoadCore();
  char ps[16]; if (GetEnvironmentVariableA("UESCAN_PORT", ps, sizeof ps)) { int p = atoi(ps); if (p > 0 && p < 65536) PORT = p; }
  WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
  SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  int one = 1; setsockopt(ls, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (char*)&one, sizeof(one));
  sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(PORT); inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
  if (bind(ls, (sockaddr*)&a, sizeof(a)) || listen(ls, 8)) { Log("listen failed %d", WSAGetLastError()); return 0; }
  Log("listening on 127.0.0.1:%d", PORT);
  for (;;) {
    SOCKET s = accept(ls, nullptr, nullptr); if (s == INVALID_SOCKET) { Sleep(100); continue; }
    HANDLE h = CreateThread(nullptr, 0, ClientThread, (LPVOID)(uintptr_t)s, 0, nullptr); if (h) CloseHandle(h);
  }
}

BOOL APIENTRY DllMain(HMODULE m, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(m);
    InitializeCriticalSection(&g_logCs); InitializeCriticalSection(&g_cmdCs);
    g_api.version = 2; g_api.log = Log; g_api.base = (uintptr_t)GetModuleHandleA(nullptr); g_api.hookPump = HookPump;
    char hv[8]; if (!GetEnvironmentVariableA("UESCAN_HOME", hv, sizeof hv)) {
      char p[MAX_PATH]; DWORD n = GetModuleFileNameA(m, p, MAX_PATH);
      if (n && n < MAX_PATH) { for (int k = 0; k < 2; k++) { char* sl = strrchr(p, '\\'); if (sl) *sl = 0; } SetEnvironmentVariableA("UESCAN_HOME", p); }
    }
    HANDLE h = CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr); if (h) CloseHandle(h);
  }
  return TRUE;
}


