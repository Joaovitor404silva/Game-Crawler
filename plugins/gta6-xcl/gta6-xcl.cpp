#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "../crawler_api.h"
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "kernel32.lib")

static const UEApi* api;
static bool g_enabled;
static DWORD g_child_pid;
static HWND g_game_window;
static wchar_t g_title[256];

static bool contains_ci(const wchar_t* text, const wchar_t* part) {
  return text && part && wcsstr(text, part) != nullptr;
}

static bool target_process() {
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  wchar_t* base = wcsrchr(exe, L'\\');
  base = base ? base + 1 : exe;
  if (_wcsicmp(base, L"xcl.exe") != 0) return false;
  return contains_ci(GetCommandLineW(), L"game_scarlett_master.exe");
}

struct ChildSearch { DWORD parent; DWORD child; };
static DWORD find_child() {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return 0;
  PROCESSENTRY32W e = {}; e.dwSize = sizeof(e);
  DWORD found = 0;
  DWORD fallback = 0;
  if (Process32FirstW(snap, &e)) do {
    if (e.th32ParentProcessID != GetCurrentProcessId()) continue;
    if (_wcsicmp(e.szExeFile, L"game_scarlett_master.exe") == 0) { found = e.th32ProcessID; break; }
    if (!fallback && _wcsicmp(e.szExeFile, L"xcl.exe") == 0) fallback = e.th32ProcessID;
  } while (Process32NextW(snap, &e));
  CloseHandle(snap);
  return found ? found : fallback;
}

static BOOL CALLBACK find_window(HWND hwnd, LPARAM param) {
  ChildSearch* s = (ChildSearch*)param;
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != s->child || !IsWindowVisible(hwnd)) return TRUE;
  wchar_t title[256] = {};
  GetWindowTextW(hwnd, title, 256);
  if (!contains_ci(title, L"Grand Theft Auto VI") && !contains_ci(title, L"GTA VI")) return TRUE;
  s->parent = pid;
  wcsncpy_s(g_title, title, _TRUNCATE);
  g_game_window = hwnd;
  return FALSE;
}

static HWND locate_game() {
  g_child_pid = find_child();
  g_game_window = nullptr;
  g_title[0] = 0;
  if (!g_child_pid) return nullptr;
  ChildSearch s = {0, g_child_pid};
  EnumWindows(find_window, (LPARAM)&s);
  return g_game_window;
}

static const char* vk_name(const char* name, WORD* vk) {
  struct Key { const char* name; WORD vk; };
  static const Key keys[] = {
    {"enter", VK_RETURN}, {"space", VK_SPACE}, {"esc", VK_ESCAPE},
    {"up", VK_UP}, {"down", VK_DOWN}, {"left", VK_LEFT}, {"right", VK_RIGHT}, {"a", 'A'},
    {"tab", VK_TAB}, {"p", 'P'}, {"backspace", VK_BACK}
  };
  for (const auto& key : keys) if (_stricmp(name, key.name) == 0) { *vk = key.vk; return key.name; }
  return nullptr;
}

static bool send_key(WORD vk, int hold_ms) {
  HWND hwnd = locate_game();
  if (!hwnd) return false;
  DWORD target_tid = GetWindowThreadProcessId(hwnd, nullptr);
  DWORD current_tid = GetCurrentThreadId();
  HWND foreground = GetForegroundWindow();
  DWORD foreground_tid = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
  bool attached_foreground = foreground_tid && foreground_tid != current_tid && AttachThreadInput(current_tid, foreground_tid, TRUE);
  bool attached_target = target_tid && target_tid != current_tid && AttachThreadInput(current_tid, target_tid, TRUE);
  ShowWindow(hwnd, SW_RESTORE);
  BringWindowToTop(hwnd);
  SetForegroundWindow(hwnd);
  SetActiveWindow(hwnd);
  SetFocus(hwnd);
  for (int i = 0; i < 20 && GetForegroundWindow() != hwnd; ++i) Sleep(25);
  bool ok = GetForegroundWindow() == hwnd;
  if (ok) {
    INPUT in = {}; in.type = INPUT_KEYBOARD; in.ki.wVk = vk;
    ok = SendInput(1, &in, sizeof(in)) == 1;
    if (ok) {
      Sleep(hold_ms);
      in.ki.dwFlags = KEYEVENTF_KEYUP;
      ok = SendInput(1, &in, sizeof(in)) == 1;
    }
  }
  if (attached_target) AttachThreadInput(current_tid, target_tid, FALSE);
  if (attached_foreground) AttachThreadInput(current_tid, foreground_tid, FALSE);
  return ok;
}

static bool click_at(int x, int y) {
  HWND hwnd = locate_game();
  if (!hwnd || x < 0 || x > 1000 || y < 0 || y > 1000) return false;
  DWORD target_tid = GetWindowThreadProcessId(hwnd, nullptr);
  DWORD current_tid = GetCurrentThreadId();
  HWND foreground = GetForegroundWindow();
  DWORD foreground_tid = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
  bool attached_foreground = foreground_tid && foreground_tid != current_tid && AttachThreadInput(current_tid, foreground_tid, TRUE);
  bool attached_target = target_tid && target_tid != current_tid && AttachThreadInput(current_tid, target_tid, TRUE);
  ShowWindow(hwnd, SW_RESTORE);
  BringWindowToTop(hwnd);
  SetForegroundWindow(hwnd);
  SetActiveWindow(hwnd);
  SetFocus(hwnd);
  for (int i = 0; i < 20 && GetForegroundWindow() != hwnd; ++i) Sleep(25);
  bool ok = GetForegroundWindow() == hwnd;
  RECT client = {};
  if (ok) ok = GetClientRect(hwnd, &client) != 0 && client.right > client.left && client.bottom > client.top;
  if (ok) {
    POINT point = {client.left + x * (client.right - client.left) / 1000,
                   client.top + y * (client.bottom - client.top) / 1000};
    ok = ClientToScreen(hwnd, &point) != 0 && SetCursorPos(point.x, point.y) != 0;
  }
  if (ok) {
    INPUT in = {}; in.type = INPUT_MOUSE; in.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    ok = SendInput(1, &in, sizeof(in)) == 1;
    if (ok) {
      Sleep(60);
      in.mi.dwFlags = MOUSEEVENTF_LEFTUP;
      ok = SendInput(1, &in, sizeof(in)) == 1;
    }
  }
  if (attached_target) AttachThreadInput(current_tid, target_tid, FALSE);
  if (attached_foreground) AttachThreadInput(current_tid, foreground_tid, FALSE);
  return ok;
}

UE_EXPORT int UEPluginInit(const UEApi* a) {
  api = a;
  g_enabled = target_process();
  api->log("gta6-xcl: %s", g_enabled ? "enabled for XCL GTA VI" : "idle (not XCL GTA VI)");
  return 1;
}

UE_EXPORT void UEPluginTick() {}
UE_EXPORT void UEPluginShutdown() {}
UE_EXPORT const char* UEPluginHelp() { return "gta6 status | gta6 tap <enter|space|esc|up|down|left|right|a|tab|p|backspace> [hold_ms] | gta6 click <x_0_1000> <y_0_1000>"; }

UE_EXPORT int UEPluginCommand(const char* line, char* out, int n) {
  if (strncmp(line, "gta6", 4) || (line[4] && line[4] != ' ')) return 0;
  if (!g_enabled) { snprintf(out, n, "gta6-xcl idle: this Game Crawler process was not launched for game_scarlett_master.exe"); return 1; }
  const char* args = line[4] ? line + 5 : "status";
  if (!strncmp(args, "status", 6) && (!args[6] || args[6] == ' ')) {
    HWND hwnd = locate_game();
    char title[768] = {};
    if (hwnd) WideCharToMultiByte(CP_UTF8, 0, g_title, -1, title, sizeof(title), nullptr, nullptr);
    snprintf(out, n, "XCL GTA VI %s; child pid=%lu; window=0x%llx; foreground=%s; title=%s",
      hwnd ? "window found" : "waiting for game window", (unsigned long)g_child_pid,
      (unsigned long long)(uintptr_t)hwnd, hwnd && GetForegroundWindow() == hwnd ? "yes" : "no",
      hwnd ? title : "");
    return 1;
  }
  if (!strncmp(args, "click ", 6)) {
    int x = -1, y = -1;
    if (sscanf_s(args + 6, "%d %d", &x, &y) != 2 || x < 0 || x > 1000 || y < 0 || y > 1000) {
      snprintf(out, n, "usage: gta6 click <x_0_1000> <y_0_1000>");
      return 1;
    }
    if (click_at(x, y)) snprintf(out, n, "clicked GTA VI window at normalized client position %d,%d", x, y);
    else snprintf(out, n, "could not focus or click the GTA VI window; run gta6 status for window discovery");
    return 1;
  }
  if (strncmp(args, "tap ", 4)) { snprintf(out, n, "usage: %s", UEPluginHelp()); return 1; }
  char key[32] = {}; int ms = 180;
  int got = sscanf_s(args + 4, "%31s %d", key, (unsigned)sizeof(key), &ms);
  WORD vk = 0;
  if (got < 1 || !vk_name(key, &vk)) { snprintf(out, n, "supported keys: enter space esc up down left right a tab p backspace"); return 1; }
  if (ms < 40) ms = 40; if (ms > 1000) ms = 1000;
  if (send_key(vk, ms)) snprintf(out, n, "sent %s for %d ms to GTA VI window (pid %lu)", key, ms, (unsigned long)g_child_pid);
  else snprintf(out, n, "could not focus/send to the GTA VI window; run gta6 status for window discovery");
  return 1;
}
