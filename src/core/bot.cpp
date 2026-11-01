#define DIRECTINPUT_VERSION 0x0800
#include <initguid.h>
#include <dinput.h>
#include "core.h"
#include "bot.h"
#include "minhook/include/MinHook.h"
#include <mutex>
#include <deque>
#include <map>
#include <atomic>
#include <thread>
#include <functional>
#include <algorithm>
#include <condition_variable>

namespace {

struct Ctr { std::atomic<long> calls{ 0 }, served{ 0 }; };
struct HookRec { std::string name; void* target; bool ok; };

std::mutex g_m;
uint8_t g_keys[256];
uint8_t g_btn[5];
bool g_cursor = false;
bool g_focus = false;
std::atomic<bool> g_diKbSeen{ false };
LONG g_cx = 0, g_cy = 0;
LONG g_diDx = 0, g_diDy = 0, g_diWheel = 0;
std::deque<DIDEVICEOBJECTDATA> g_diKb, g_diMouse;
DWORD g_diSeq = 0;
RAWINPUT g_raw[256];
uint32_t g_rawSeq = 0;
const uintptr_t kFakeRaw = 0x7B0000000000ULL;
std::atomic<unsigned> g_via{ bot::ALL };
HWND g_hwnd = nullptr;
std::map<void*, int> g_devType;
std::map<void*, long> g_devCalls;
HRESULT g_lastDiHr = 0; DWORD g_lastCbObj = 0, g_lastCap = 0, g_lastFlags = 0;
std::vector<HookRec> g_hooks;
bool g_userHooked = false, g_diHooked = false;
Ctr c_async, c_keyState, c_kbState, c_cursorPos, c_raw, c_diState, c_diData;
std::atomic<long> c_posted{ 0 };

std::mutex g_jm;
std::condition_variable g_jcv;
std::deque<std::function<void()>> g_jobs;
std::thread g_thread;
std::atomic<bool> g_stop{ false };
std::atomic<long> g_epoch{ 0 };
HANDLE g_wake = nullptr;

typedef SHORT(WINAPI* KeyFn)(int);
typedef BOOL(WINAPI* KbFn)(PBYTE);
typedef BOOL(WINAPI* CurFn)(LPPOINT);
typedef UINT(WINAPI* RawFn)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
typedef HRESULT(WINAPI* GdsFn)(void*, DWORD, LPVOID);
typedef HRESULT(WINAPI* GddFn)(void*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
KeyFn o_async, o_keyState;
KbFn o_kbState;
CurFn o_cursorPos;
RawFn o_raw;
typedef HWND(WINAPI* WndFn)();
WndFn o_fg, o_active, o_getFocus;
GdsFn o_gdsW, o_gdsA, o_gdsW2, o_gdsA2;
GddFn o_gddW, o_gddA, o_gddW2, o_gddA2;

BOOL CALLBACK FindWnd(HWND h, LPARAM lp) {
  DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
  if (pid != GetCurrentProcessId() || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
  RECT r; GetClientRect(h, &r); auto* best = (std::pair<HWND, long>*)lp; long area = (r.right - r.left) * (r.bottom - r.top);
  if (area > best->second) *best = { h, area };
  return TRUE;
}

HWND Wnd() {
  if (g_hwnd && IsWindow(g_hwnd)) return g_hwnd;
  std::pair<HWND, long> best{ nullptr, -1 }; EnumWindows(FindWnd, (LPARAM)&best); g_hwnd = best.first; return g_hwnd;
}

bool Down(int vk) {
  if (vk < 0 || vk > 255) return false;
  if (g_keys[vk]) return true;
  switch (vk) {
  case VK_LBUTTON: return g_btn[0] != 0;
  case VK_RBUTTON: return g_btn[1] != 0;
  case VK_MBUTTON: return g_btn[2] != 0;
  case VK_XBUTTON1: return g_btn[3] != 0;
  case VK_XBUTTON2: return g_btn[4] != 0;
  case VK_SHIFT: return g_keys[VK_LSHIFT] || g_keys[VK_RSHIFT];
  case VK_CONTROL: return g_keys[VK_LCONTROL] || g_keys[VK_RCONTROL];
  case VK_MENU: return g_keys[VK_LMENU] || g_keys[VK_RMENU];
  default: return false;
  }
}

SHORT WINAPI HkAsync(int vk) {
  SHORT r = o_async(vk); c_async.calls++;
  if ((g_via & bot::POLL) && Down(vk)) { c_async.served++; r |= (SHORT)0x8000; }
  return r;
}
SHORT WINAPI HkKeyState(int vk) {
  SHORT r = o_keyState(vk); c_keyState.calls++;
  if ((g_via & bot::POLL) && Down(vk)) { c_keyState.served++; r |= (SHORT)0x8000; }
  return r;
}
BOOL WINAPI HkKbState(PBYTE s) {
  BOOL r = o_kbState(s); c_kbState.calls++;
  if (r && s && (g_via & bot::POLL)) { bool any = false; for (int i = 1; i < 256; i++) if (Down(i)) { s[i] |= 0x80; any = true; } if (any) c_kbState.served++; }
  return r;
}
BOOL WINAPI HkCursorPos(LPPOINT p) {
  BOOL r = o_cursorPos(p); c_cursorPos.calls++;
  if (r && p && g_cursor && (g_via & bot::POLL)) { HWND h = Wnd(); if (h) { POINT q{ g_cx, g_cy }; ClientToScreen(h, &q); *p = q; c_cursorPos.served++; } }
  return r;
}
HWND RealFg() { return o_fg ? o_fg() : GetForegroundWindow(); }
HWND WINAPI HkFg() { HWND h = o_fg(); return g_focus && g_hwnd ? g_hwnd : h; }
HWND WINAPI HkActive() { HWND h = o_active(); return g_focus && g_hwnd ? g_hwnd : h; }
HWND WINAPI HkGetFocus() { HWND h = o_getFocus(); return g_focus && g_hwnd ? g_hwnd : h; }

UINT WINAPI HkRaw(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT hdr) {
  c_raw.calls++;
  if (((uintptr_t)h & ~(uintptr_t)0xff) != kFakeRaw) return o_raw(h, cmd, data, size, hdr);
  RAWINPUT ri; { std::lock_guard<std::mutex> l(g_m); ri = g_raw[(uintptr_t)h & 0xff]; }
  UINT need = cmd == RID_HEADER ? (UINT)sizeof(RAWINPUTHEADER) : ri.header.dwSize;
  if (!size) return (UINT)-1;
  if (!data) { *size = need; return 0; }
  if (*size < need) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return (UINT)-1; }
  memcpy(data, &ri, need); c_raw.served++; return need;
}

int DevType(void* dev) {
  { std::lock_guard<std::mutex> l(g_m); auto it = g_devType.find(dev); if (it != g_devType.end()) return it->second; }
  DIDEVCAPS caps{}; caps.dwSize = sizeof caps; int t = 0;
  if (SUCCEEDED(((IDirectInputDevice8W*)dev)->GetCapabilities(&caps))) t = GET_DIDEVICE_TYPE(caps.dwDevType);
  std::lock_guard<std::mutex> l(g_m); g_devType[dev] = t; return t;
}

void PatchState(DWORD cb, LPVOID data) {
  if (!data || !(g_via & bot::DI)) return;
  if (cb == 256) {
    uint8_t* k = (uint8_t*)data; bool any = false;
    for (int vk = 8; vk < 256; vk++) if (g_keys[vk]) { int sc = (int)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC); if (sc) { k[bot::DikOf(sc, vk)] = 0x80; any = true; } }
    if (any) c_diState.served++;
  } else if (cb == sizeof(DIMOUSESTATE) || cb == sizeof(DIMOUSESTATE2)) {
    DIMOUSESTATE2* m = (DIMOUSESTATE2*)data; int nb = cb == sizeof(DIMOUSESTATE) ? 4 : 8; bool any = false;
    { std::lock_guard<std::mutex> l(g_m); if (g_diDx || g_diDy || g_diWheel) { m->lX += g_diDx; m->lY += g_diDy; m->lZ += g_diWheel; g_diDx = g_diDy = g_diWheel = 0; any = true; } }
    for (int i = 0; i < 5 && i < nb; i++) if (g_btn[i]) { m->rgbButtons[i] = 0x80; any = true; }
    if (any) c_diState.served++;
  }
}
bool Unfocused(HRESULT hr) { return g_focus && (g_via & bot::DI) && (hr == DIERR_NOTACQUIRED || hr == DIERR_INPUTLOST || hr == DIERR_OTHERAPPHASPRIO); }
HRESULT Gds(GdsFn o, void* dev, DWORD cb, LPVOID data) {
  HRESULT hr = o(dev, cb, data); c_diState.calls++;
  if (FAILED(hr) && data && Unfocused(hr)) { memset(data, 0, cb); hr = DI_OK; }
  if (SUCCEEDED(hr)) PatchState(cb, data); return hr;
}
HRESULT WINAPI HkGdsW(void* dev, DWORD cb, LPVOID data) { return Gds(o_gdsW, dev, cb, data); }
HRESULT WINAPI HkGdsA(void* dev, DWORD cb, LPVOID data) { return Gds(o_gdsA, dev, cb, data); }
HRESULT WINAPI HkGdsW2(void* dev, DWORD cb, LPVOID data) { return Gds(o_gdsW2, dev, cb, data); }
HRESULT WINAPI HkGdsA2(void* dev, DWORD cb, LPVOID data) { return Gds(o_gdsA2, dev, cb, data); }

void AppendData(void* dev, DWORD cbObj, LPDIDEVICEOBJECTDATA rg, LPDWORD inout, DWORD cap) {
  int t = DevType(dev); if (t == DI8DEVTYPE_KEYBOARD) g_diKbSeen = true; std::lock_guard<std::mutex> l(g_m);
  auto* q = t == DI8DEVTYPE_KEYBOARD ? &g_diKb : t == DI8DEVTYPE_MOUSE ? &g_diMouse : nullptr; if (!q) return;
  while (*inout < cap && !q->empty()) {
    DIDEVICEOBJECTDATA d = q->front(); q->pop_front();
    memcpy((uint8_t*)rg + (size_t)*inout * cbObj, &d, std::min<size_t>(cbObj, sizeof d)); (*inout)++; c_diData.served++;
  }
}
HRESULT Gdd(GddFn o, void* dev, DWORD cbObj, LPDIDEVICEOBJECTDATA rg, LPDWORD inout, DWORD flags) {
  DWORD cap = inout ? *inout : 0; HRESULT hr = o(dev, cbObj, rg, inout, flags); c_diData.calls++;
  { std::lock_guard<std::mutex> l(g_m); g_devCalls[dev]++; g_lastDiHr = hr; g_lastCbObj = cbObj; g_lastCap = cap; g_lastFlags = flags; }
  if (FAILED(hr) && inout && Unfocused(hr)) { *inout = 0; hr = DI_OK; }
  if (SUCCEEDED(hr) && rg && inout && !(flags & DIGDD_PEEK) && (g_via & bot::DI) && cbObj >= 16) AppendData(dev, cbObj, rg, inout, cap);
  return hr;
}
HRESULT WINAPI HkGddW(void* dev, DWORD cbObj, LPDIDEVICEOBJECTDATA rg, LPDWORD inout, DWORD flags) { return Gdd(o_gddW, dev, cbObj, rg, inout, flags); }
HRESULT WINAPI HkGddA(void* dev, DWORD cbObj, LPDIDEVICEOBJECTDATA rg, LPDWORD inout, DWORD flags) { return Gdd(o_gddA, dev, cbObj, rg, inout, flags); }
HRESULT WINAPI HkGddW2(void* dev, DWORD cbObj, LPDIDEVICEOBJECTDATA rg, LPDWORD inout, DWORD flags) { return Gdd(o_gddW2, dev, cbObj, rg, inout, flags); }
HRESULT WINAPI HkGddA2(void* dev, DWORD cbObj, LPDIDEVICEOBJECTDATA rg, LPDWORD inout, DWORD flags) { return Gdd(o_gddA2, dev, cbObj, rg, inout, flags); }

bool Hook(const char* name, void* target, void* detour, void** orig) {
  if (!target) { g_hooks.push_back({ name, nullptr, false }); return false; }
  for (auto& h : g_hooks) if (h.target == target && h.ok) return true;
  MH_STATUS s = MH_CreateHook(target, detour, orig); if (s == MH_OK) s = MH_EnableHook(target);
  if (s != MH_OK) MH_RemoveHook(target);
  g_hooks.push_back({ name, target, s == MH_OK }); LOG("bot hook %s %s", name, s == MH_OK ? "ok" : MH_StatusToString(s));
  return s == MH_OK;
}

void InstallUser() {
  if (g_userHooked) return; g_userHooked = true;
  HMODULE u = GetModuleHandleA("user32.dll"); if (!u) u = LoadLibraryA("user32.dll");
  Hook("GetAsyncKeyState", (void*)GetProcAddress(u, "GetAsyncKeyState"), (void*)HkAsync, (void**)&o_async);
  Hook("GetKeyState", (void*)GetProcAddress(u, "GetKeyState"), (void*)HkKeyState, (void**)&o_keyState);
  Hook("GetKeyboardState", (void*)GetProcAddress(u, "GetKeyboardState"), (void*)HkKbState, (void**)&o_kbState);
  Hook("GetCursorPos", (void*)GetProcAddress(u, "GetCursorPos"), (void*)HkCursorPos, (void**)&o_cursorPos);
  Hook("GetForegroundWindow", (void*)GetProcAddress(u, "GetForegroundWindow"), (void*)HkFg, (void**)&o_fg);
  Hook("GetActiveWindow", (void*)GetProcAddress(u, "GetActiveWindow"), (void*)HkActive, (void**)&o_active);
  Hook("GetFocus", (void*)GetProcAddress(u, "GetFocus"), (void*)HkGetFocus, (void**)&o_getFocus);
  Hook("GetRawInputData", (void*)GetProcAddress(u, "GetRawInputData"), (void*)HkRaw, (void**)&o_raw);
}

void InstallDi() {
  if (g_diHooked) return;
  HMODULE d = GetModuleHandleA("dinput8.dll"); if (!d) return;
  typedef HRESULT(WINAPI* CreateFn)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
  auto create = (CreateFn)GetProcAddress(d, "DirectInput8Create"); if (!create) return;
  g_diHooked = true;
  void* vt[2][2][2] = {};
  const GUID* guids[2] = { &GUID_SysKeyboard, &GUID_SysMouse };
  IDirectInput8W* w = nullptr;
  if (SUCCEEDED(create(GetModuleHandleA(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W, (LPVOID*)&w, nullptr)) && w) {
    for (int g = 0; g < 2; g++) { IDirectInputDevice8W* dev = nullptr; if (SUCCEEDED(w->CreateDevice(*guids[g], &dev, nullptr)) && dev) { void** v = *(void***)dev; vt[0][g][0] = v[9]; vt[0][g][1] = v[10]; dev->Release(); } }
    w->Release();
  }
  IDirectInput8A* an = nullptr;
  if (SUCCEEDED(create(GetModuleHandleA(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8A, (LPVOID*)&an, nullptr)) && an) {
    for (int g = 0; g < 2; g++) { IDirectInputDevice8A* dev = nullptr; if (SUCCEEDED(an->CreateDevice(*guids[g], &dev, nullptr)) && dev) { void** v = *(void***)dev; vt[1][g][0] = v[9]; vt[1][g][1] = v[10]; dev->Release(); } }
    an->Release();
  }
  GdsFn* gdsSlots[4] = { &o_gdsW, &o_gdsA, &o_gdsW2, &o_gdsA2 }; GddFn* gddSlots[4] = { &o_gddW, &o_gddA, &o_gddW2, &o_gddA2 };
  void* gdsHk[4] = { (void*)HkGdsW, (void*)HkGdsA, (void*)HkGdsW2, (void*)HkGdsA2 }; void* gddHk[4] = { (void*)HkGddW, (void*)HkGddA, (void*)HkGddW2, (void*)HkGddA2 };
  std::vector<void*> seenS, seenD; int ns = 0, nd = 0;
  for (int i = 0; i < 2; i++) for (int g = 0; g < 2; g++) {
    void* ps = vt[i][g][0]; void* pd = vt[i][g][1];
    if (ps && ns < 4 && std::find(seenS.begin(), seenS.end(), ps) == seenS.end()) { seenS.push_back(ps); Hook(Fmt("DI8.GetDeviceState(%s%s)", i ? "A" : "W", g ? ",mouse" : "").c_str(), ps, gdsHk[ns], (void**)gdsSlots[ns]); ns++; }
    if (pd && nd < 4 && std::find(seenD.begin(), seenD.end(), pd) == seenD.end()) { seenD.push_back(pd); Hook(Fmt("DI8.GetDeviceData(%s%s)", i ? "A" : "W", g ? ",mouse" : "").c_str(), pd, gddHk[nd], (void**)gddSlots[nd]); nd++; }
  }
}

void Install() { InstallUser(); InstallDi(); }

void Post(UINT msg, WPARAM w, LPARAM l) { HWND h = Wnd(); if (h && PostMessageW(h, msg, w, l)) c_posted++; }

void PostRaw(const RAWINPUT& ri) {
  uintptr_t idx; { std::lock_guard<std::mutex> l(g_m); idx = g_rawSeq++ & 0xff; g_raw[idx] = ri; }
  Post(WM_INPUT, RIM_INPUT, (LPARAM)(kFakeRaw | idx));
}

void PushDi(std::deque<DIDEVICEOBJECTDATA>& q, DWORD ofs, DWORD data) {
  DIDEVICEOBJECTDATA d{}; d.dwOfs = ofs; d.dwData = data; d.dwTimeStamp = GetTickCount(); d.dwSequence = ++g_diSeq; d.uAppData = (UINT_PTR)-1;
  q.push_back(d); while (q.size() > 512) q.pop_front();
}

int GenericVk(int vk) {
  if (vk == VK_LSHIFT || vk == VK_RSHIFT) return VK_SHIFT;
  if (vk == VK_LCONTROL || vk == VK_RCONTROL) return VK_CONTROL;
  if (vk == VK_LMENU || vk == VK_RMENU) return VK_MENU;
  return vk;
}

WPARAM MkFlags() {
  WPARAM w = 0;
  if (g_btn[0]) w |= MK_LBUTTON; if (g_btn[1]) w |= MK_RBUTTON; if (g_btn[2]) w |= MK_MBUTTON; if (g_btn[3]) w |= MK_XBUTTON1; if (g_btn[4]) w |= MK_XBUTTON2;
  if (Down(VK_SHIFT)) w |= MK_SHIFT; if (Down(VK_CONTROL)) w |= MK_CONTROL;
  return w;
}

POINT CursorClient() {
  if (g_cursor) return { g_cx, g_cy };
  POINT p{}; if (o_cursorPos) o_cursorPos(&p); else GetCursorPos(&p); HWND h = Wnd(); if (h) ScreenToClient(h, &p); return p;
}

void Button(int i, bool down, unsigned via);

void KeyEvent(int vk, bool down, unsigned via, bool msg = true) {
  if (vk >= VK_LBUTTON && vk <= VK_XBUTTON2 && vk != VK_CANCEL) {
    int b = vk == VK_LBUTTON ? 0 : vk == VK_RBUTTON ? 1 : vk == VK_MBUTTON ? 2 : vk == VK_XBUTTON1 ? 3 : 4; Button(b, down, via); return;
  }
  int scan = (int)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC); int gv = GenericVk(vk);
  { std::lock_guard<std::mutex> l(g_m); g_keys[vk] = down ? 1 : 0; if ((via & bot::DI) && scan) PushDi(g_diKb, bot::DikOf(scan, vk), down ? 0x80 : 0); }
  if (via & bot::RAW) {
    RAWINPUT ri{}; ri.header.dwType = RIM_TYPEKEYBOARD; ri.header.dwSize = sizeof(RAWINPUTHEADER) + sizeof(RAWKEYBOARD); ri.header.wParam = RIM_INPUT;
    ri.data.keyboard.MakeCode = (USHORT)scan; ri.data.keyboard.Flags = (USHORT)((down ? RI_KEY_MAKE : RI_KEY_BREAK) | (bot::IsExtended(vk) ? RI_KEY_E0 : 0));
    ri.data.keyboard.VKey = (USHORT)gv; ri.data.keyboard.Message = down ? WM_KEYDOWN : WM_KEYUP; PostRaw(ri);
  }
  if ((via & bot::MSG) && msg && !(g_diKbSeen && (via & bot::DI))) {
    bool sys = Down(VK_MENU) && gv != VK_MENU;
    Post(down ? (sys ? WM_SYSKEYDOWN : WM_KEYDOWN) : (sys ? WM_SYSKEYUP : WM_KEYUP), gv, bot::KeyLParam(scan, vk, down));
  }
}

void Button(int i, bool down, unsigned via) {
  if (i < 0 || i > 4) return;
  { std::lock_guard<std::mutex> l(g_m); g_btn[i] = down ? 1 : 0; if (via & bot::DI) PushDi(g_diMouse, DIMOFS_BUTTON0 + i, down ? 0x80 : 0); }
  if (via & bot::RAW) {
    RAWINPUT ri{}; ri.header.dwType = RIM_TYPEMOUSE; ri.header.dwSize = sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE); ri.header.wParam = RIM_INPUT;
    ri.data.mouse.usFlags = MOUSE_MOVE_RELATIVE; ri.data.mouse.usButtonFlags = (USHORT)(1 << (i * 2 + (down ? 0 : 1))); PostRaw(ri);
  }
  if (via & bot::MSG) {
    POINT p = CursorClient(); LPARAM lp = MAKELPARAM(p.x, p.y);
    static const UINT dn[5] = { WM_LBUTTONDOWN, WM_RBUTTONDOWN, WM_MBUTTONDOWN, WM_XBUTTONDOWN, WM_XBUTTONDOWN };
    static const UINT up[5] = { WM_LBUTTONUP, WM_RBUTTONUP, WM_MBUTTONUP, WM_XBUTTONUP, WM_XBUTTONUP };
    WPARAM w = MkFlags(); if (i >= 3) w |= (WPARAM)(i == 3 ? XBUTTON1 : XBUTTON2) << 16;
    Post(down ? dn[i] : up[i], w, lp);
  }
}

void MoveRel(LONG dx, LONG dy, unsigned via) {
  bool cur;
  {
    std::lock_guard<std::mutex> l(g_m); cur = g_cursor;
    if (cur) { g_cx += dx; g_cy += dy; RECT r{}; HWND h = g_hwnd; if (h && GetClientRect(h, &r)) { g_cx = std::max<LONG>(0, std::min<LONG>(r.right - 1, g_cx)); g_cy = std::max<LONG>(0, std::min<LONG>(r.bottom - 1, g_cy)); } }
    if (via & bot::DI) { g_diDx += dx; g_diDy += dy; if (dx) PushDi(g_diMouse, DIMOFS_X, (DWORD)dx); if (dy) PushDi(g_diMouse, DIMOFS_Y, (DWORD)dy); }
  }
  if (via & bot::RAW) {
    RAWINPUT ri{}; ri.header.dwType = RIM_TYPEMOUSE; ri.header.dwSize = sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE); ri.header.wParam = RIM_INPUT;
    ri.data.mouse.usFlags = MOUSE_MOVE_RELATIVE; ri.data.mouse.lLastX = dx; ri.data.mouse.lLastY = dy; PostRaw(ri);
  }
  if ((via & bot::MSG) && cur) Post(WM_MOUSEMOVE, MkFlags(), MAKELPARAM(g_cx, g_cy));
}

void MoveTo(LONG x, LONG y, unsigned via) {
  POINT p = CursorClient();
  { std::lock_guard<std::mutex> l(g_m); if (!g_cursor) { g_cx = p.x; g_cy = p.y; g_cursor = true; } }
  MoveRel(x - g_cx, y - g_cy, via);
  std::lock_guard<std::mutex> l(g_m); g_cx = x; g_cy = y;
}

void Wheel(int n, unsigned via) {
  { std::lock_guard<std::mutex> l(g_m); if (via & bot::DI) { g_diWheel += n * WHEEL_DELTA; PushDi(g_diMouse, DIMOFS_Z, (DWORD)(n * WHEEL_DELTA)); } }
  if (via & bot::RAW) {
    RAWINPUT ri{}; ri.header.dwType = RIM_TYPEMOUSE; ri.header.dwSize = sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE); ri.header.wParam = RIM_INPUT;
    ri.data.mouse.usButtonFlags = RI_MOUSE_WHEEL; ri.data.mouse.usButtonData = (USHORT)(SHORT)(n * WHEEL_DELTA); PostRaw(ri);
  }
  if (via & bot::MSG) { POINT p = CursorClient(); HWND h = Wnd(); if (h) ClientToScreen(h, &p); Post(WM_MOUSEWHEEL, MAKEWPARAM(MkFlags(), n * WHEEL_DELTA), MAKELPARAM(p.x, p.y)); }
}

void ReleaseAll() {
  unsigned via = g_via;
  for (int vk = 1; vk < 256; vk++) if (g_keys[vk]) KeyEvent(vk, false, via);
  for (int i = 0; i < 5; i++) if (g_btn[i]) Button(i, false, via);
  std::lock_guard<std::mutex> l(g_m); g_cursor = false; g_diDx = g_diDy = g_diWheel = 0;
}

bool Pause(long epoch, int ms) {
  if (ms > 0) WaitForSingleObject(g_wake, (DWORD)ms);
  return !g_stop && g_epoch == epoch;
}

void Worker() {
  for (;;) {
    std::function<void()> j;
    { std::unique_lock<std::mutex> l(g_jm); g_jcv.wait(l, [] { return g_stop || !g_jobs.empty(); }); if (g_stop) return; j = std::move(g_jobs.front()); g_jobs.pop_front(); }
    j();
  }
}

struct Opts { int ms = -1, steps = 1; bool wait = false, dbl = false; unsigned via = 0; std::vector<std::string> pos; std::string err; };

Opts ParseOpts(const std::vector<std::string>& t, size_t from) {
  Opts o;
  for (size_t i = from; i < t.size(); i++) {
    const std::string& s = t[i];
    if (s == "wait") o.wait = true;
    else if (s == "double") o.dbl = true;
    else if (s.rfind("ms=", 0) == 0) o.ms = atoi(s.c_str() + 3);
    else if (s.rfind("steps=", 0) == 0) o.steps = std::max(1, atoi(s.c_str() + 6));
    else if (s.rfind("via=", 0) == 0) { o.via = bot::ParseVia(s.substr(4)); if (!o.via) o.err = "via must be a list of msg,poll,raw,di,all"; }
    else o.pos.push_back(s);
  }
  return o;
}

void Run(Out& out, const Opts& o, const std::string& what, std::function<void(long, unsigned)> fn) {
  HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr); long ep = g_epoch; unsigned via = o.via ? o.via : (unsigned)g_via;
  HANDLE w = nullptr; if (o.wait) DuplicateHandle(GetCurrentProcess(), done, GetCurrentProcess(), &w, 0, FALSE, DUPLICATE_SAME_ACCESS);
  { std::lock_guard<std::mutex> l(g_jm); g_jobs.push_back([fn, done, ep, via] { if (g_epoch == ep && !g_stop) fn(ep, via); SetEvent(done); CloseHandle(done); }); }
  g_jcv.notify_one();
  if (w) { DWORD r = WaitForSingleObject(w, 30000); CloseHandle(w); out.line(what + (r == WAIT_OBJECT_0 ? " done" : " still running after 30 s")); }
  else out.line(what + " queued");
}

std::string HookSummary() {
  std::string s; for (auto& h : g_hooks) s += Fmt("%s%s%s", s.empty() ? "" : ", ", h.name.c_str(), h.ok ? "" : "(failed)"); return s.empty() ? "none" : s;
}

void Status(Out& o) {
  HWND h = Wnd(); RECT r{}; if (h) GetClientRect(h, &r); wchar_t title[128] = {}; if (h) GetWindowTextW(h, title, 127);
  unsigned v = g_via;
  o.f("bot window 0x%llx \"%s\" client %ldx%ld foreground=%s", (unsigned long long)(uintptr_t)h, Narrow(title).c_str(), r.right, r.bottom, h && RealFg() == h ? "yes" : "no");
  o.f("focus spoof %s  keyboard %s", g_focus ? "on" : "off", g_diKbSeen ? "DirectInput (key taps skip WM_KEYDOWN)" : "messages/poll/raw");
  o.f("via %s%s%s%s  cursor %s", v & bot::MSG ? "msg " : "", v & bot::POLL ? "poll " : "", v & bot::RAW ? "raw " : "", v & bot::DI ? "di" : "", g_cursor ? Fmt("on at %ld,%ld", g_cx, g_cy).c_str() : "off (real cursor)");
  std::string held; for (int i = 1; i < 256; i++) if (g_keys[i]) held += Fmt(" 0x%02x", i); for (int i = 0; i < 5; i++) if (g_btn[i]) held += Fmt(" btn%d", i);
  o.f("held:%s", held.empty() ? " none" : held.c_str());
  o.f("hooks: %s", HookSummary().c_str());
  { std::string d; std::lock_guard<std::mutex> l(g_m); for (auto& e : g_devCalls) { auto it = g_devType.find(e.first); d += Fmt(" %p type=0x%x calls=%ld", e.first, it == g_devType.end() ? -1 : it->second, e.second); }
    o.f("di devices:%s  last hr=0x%lx cbObj=%lu cap=%lu flags=%lu  queued kb=%zu mouse=%zu", d.empty() ? " none" : d.c_str(), (unsigned long)g_lastDiHr, g_lastCbObj, g_lastCap, g_lastFlags, g_diKb.size(), g_diMouse.size()); }
  o.f("game reads (calls/injected): GetAsyncKeyState %ld/%ld  GetKeyState %ld/%ld  GetKeyboardState %ld/%ld  GetCursorPos %ld/%ld  GetRawInputData %ld/%ld  DI state %ld/%ld  DI data %ld/%ld  posted msgs %ld",
    (long)c_async.calls, (long)c_async.served, (long)c_keyState.calls, (long)c_keyState.served, (long)c_kbState.calls, (long)c_kbState.served,
    (long)c_cursorPos.calls, (long)c_cursorPos.served, (long)c_raw.calls, (long)c_raw.served, (long)c_diState.calls, (long)c_diState.served,
    (long)c_diData.calls, (long)c_diData.served, (long)c_posted);
}

const char* kUsage =
  "bot status | bot focus [on|off] | bot key <key> [tap|down|up] [ms=80] | bot text <string> [ms=30] | bot move <dx> <dy> [steps=1] [ms=0] | bot moveto <x> <y> [steps=1] [ms=0] | "
  "bot click [left|right|middle|x1|x2] [<x> <y>] [ms=60] [double] | bot press|release-button <button> | bot wheel <n> | bot home | bot release | bot via <msg,poll,raw,di|all> | bot cursor off\n"
  "  common: [wait] blocks until done; [via=msg,poll,raw,di] limits channels for one command. Keys: a-z 0-9 f1-f24 space enter esc tab shift ctrl alt up down left right ... or 0x<vk>";

}

void InitBot() {
  g_stop = false; g_wake = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  g_thread = std::thread(Worker);
}

void ShutdownBot() {
  g_stop = true; g_epoch++; if (g_wake) SetEvent(g_wake); g_jcv.notify_all();
  if (g_thread.joinable()) g_thread.join();
  { std::lock_guard<std::mutex> l(g_jm); g_jobs.clear(); }
  ReleaseAll();
  for (auto& h : g_hooks) if (h.ok) { MH_DisableHook(h.target); MH_RemoveHook(h.target); }
  g_hooks.clear(); g_userHooked = g_diHooked = false;
  if (g_wake) { CloseHandle(g_wake); g_wake = nullptr; }
}

void Activate(bool on) {
  HWND h = Wnd(); if (!h) return;
  PostMessageW(h, WM_ACTIVATEAPP, on ? TRUE : FALSE, 0); PostMessageW(h, WM_NCACTIVATE, on ? TRUE : FALSE, 0);
  PostMessageW(h, WM_ACTIVATE, on ? WA_ACTIVE : WA_INACTIVE, 0); PostMessageW(h, on ? WM_SETFOCUS : WM_KILLFOCUS, 0, 0);
}

bool HandleBot(const std::string& c, const std::vector<std::string>& t, Out& o) {
  if (c != "bot") return false;
  if (t.size() < 2) { o.line(kUsage); return true; }
  std::string a = Lower(t[1]);
  Install();
  if (g_focus && Lower(t[1]) != "focus" && Wnd() && RealFg() != Wnd()) Activate(true);
  if (a == "status") { Status(o); return true; }
  if (a == "via") { unsigned v = t.size() > 2 ? bot::ParseVia(Lower(t[2])) : 0; if (!v) { o.line("usage: bot via <msg,poll,raw,di|all>"); return true; } g_via = v; Status(o); return true; }
  if (a == "focus") {
    bool on = t.size() < 3 || Lower(t[2]) != "off"; g_focus = on; Activate(on);
    o.f("bot focus %s (game believes it is the foreground window)", on ? "on" : "off"); return true;
  }
  if (a == "cursor") { std::lock_guard<std::mutex> l(g_m); g_cursor = false; o.line("bot cursor off (game sees the real cursor)"); return true; }
  if (a == "release" || a == "stop") {
    g_epoch++; SetEvent(g_wake); { std::lock_guard<std::mutex> l(g_jm); g_jobs.clear(); }
    Opts op; op.wait = true; Run(o, op, "release", [](long, unsigned) { ResetEvent(g_wake); ReleaseAll(); }); return true;
  }
  Opts op = ParseOpts(t, 2);
  if (!op.err.empty()) { o.line("error: " + op.err); return true; }
  if (!Wnd()) { o.line("error: no visible game window in this process"); return true; }
  if (a == "key") {
    if (op.pos.empty()) { o.line(kUsage); return true; }
    int vk = bot::ParseKey(op.pos[0]); if (!vk) { o.line("error: unknown key " + op.pos[0]); return true; }
    std::string mode = op.pos.size() > 1 ? Lower(op.pos[1]) : "tap"; int ms = op.ms < 0 ? 80 : op.ms;
    if (mode != "tap" && mode != "down" && mode != "up") { o.line("error: mode must be tap|down|up"); return true; }
    Run(o, op, Fmt("key %s %s", op.pos[0].c_str(), mode.c_str()), [vk, mode, ms](long ep, unsigned via) {
      if (mode != "up") KeyEvent(vk, true, via);
      if (mode == "tap") { Pause(ep, ms); KeyEvent(vk, false, via); }
      if (mode == "up") KeyEvent(vk, false, via);
    });
    return true;
  }
  if (a == "text") {
    if (op.pos.empty()) { o.line(kUsage); return true; }
    std::string s; for (size_t i = 0; i < op.pos.size(); i++) s += (i ? " " : "") + op.pos[i];
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
    std::wstring w = Widen(s); int ms = op.ms < 0 ? 30 : op.ms;
    Run(o, op, Fmt("text (%zu chars)", w.size()), [w, ms](long ep, unsigned via) {
      for (wchar_t ch : w) {
        if (!Pause(ep, 0)) return;
        SHORT k = VkKeyScanW(ch);
        if (k == -1) { if (via & bot::MSG) Post(WM_CHAR, ch, 1); Pause(ep, ms); continue; }
        int vk = k & 0xff, sh = (k >> 8) & 0xff;
        if (sh & 1) KeyEvent(VK_LSHIFT, true, via, false); if (sh & 2) KeyEvent(VK_LCONTROL, true, via, false); if (sh & 4) KeyEvent(VK_LMENU, true, via, false);
        KeyEvent(vk, true, via, false); if (via & bot::MSG) Post(WM_CHAR, ch, 1);
        Pause(ep, ms); KeyEvent(vk, false, via, false);
        if (sh & 4) KeyEvent(VK_LMENU, false, via, false); if (sh & 2) KeyEvent(VK_LCONTROL, false, via, false); if (sh & 1) KeyEvent(VK_LSHIFT, false, via, false);
        Pause(ep, ms);
      }
    });
    return true;
  }
  if (a == "move" || a == "moveto") {
    if (op.pos.size() < 2) { o.line(kUsage); return true; }
    LONG x = atol(op.pos[0].c_str()), y = atol(op.pos[1].c_str()); int steps = op.steps, ms = op.ms < 0 ? 0 : op.ms; bool abs = a == "moveto";
    Run(o, op, Fmt("%s %ld %ld", a.c_str(), x, y), [x, y, steps, ms, abs](long ep, unsigned via) {
      if (abs) {
        POINT p = CursorClient(); LONG sx = g_cursor ? g_cx : p.x, sy = g_cursor ? g_cy : p.y;
        for (int i = 1; i <= steps; i++) { MoveTo(sx + (x - sx) * i / steps, sy + (y - sy) * i / steps, via); if (i < steps && !Pause(ep, ms)) return; }
      } else {
        LONG dx = 0, dy = 0;
        for (int i = 1; i <= steps; i++) { LONG nx = x * i / steps, ny = y * i / steps; MoveRel(nx - dx, ny - dy, via); dx = nx; dy = ny; if (i < steps && !Pause(ep, ms)) return; }
      }
    });
    return true;
  }
  if (a == "home") {
    Run(o, op, "home", [](long ep, unsigned via) {
      for (int i = 0; i < 20; i++) { MoveRel(-2000, -2000, via & ~bot::MSG); if (!Pause(ep, 10)) return; }
      std::lock_guard<std::mutex> l(g_m); g_cursor = true; g_cx = 0; g_cy = 0;
    });
    return true;
  }
  if (a == "click") {
    int b = 0; size_t k = 0;
    if (!op.pos.empty() && bot::ButtonIndex(Lower(op.pos[0])) >= 0) { b = bot::ButtonIndex(Lower(op.pos[0])); k = 1; }
    bool at = op.pos.size() >= k + 2; LONG x = at ? atol(op.pos[k].c_str()) : 0, y = at ? atol(op.pos[k + 1].c_str()) : 0;
    int ms = op.ms < 0 ? 60 : op.ms, n = op.dbl ? 2 : 1;
    Run(o, op, at ? Fmt("click %d at %ld,%ld", b, x, y) : Fmt("click %d", b), [b, at, x, y, ms, n](long ep, unsigned via) {
      if (at) { MoveTo(x, y, via); if (!Pause(ep, 50)) return; }
      for (int i = 0; i < n; i++) { Button(b, true, via); Pause(ep, ms); Button(b, false, via); if (i + 1 < n && !Pause(ep, 80)) return; }
    });
    return true;
  }
  if (a == "press" || a == "release-button" || a == "mousedown" || a == "mouseup") {
    int b = op.pos.empty() ? 0 : bot::ButtonIndex(Lower(op.pos[0])); if (b < 0) { o.line("error: button must be left|right|middle|x1|x2"); return true; }
    bool dn = a == "press" || a == "mousedown";
    Run(o, op, Fmt("button %d %s", b, dn ? "down" : "up"), [b, dn](long, unsigned via) { Button(b, dn, via); });
    return true;
  }
  if (a == "wheel") {
    if (op.pos.empty()) { o.line(kUsage); return true; }
    int n = atoi(op.pos[0].c_str());
    Run(o, op, Fmt("wheel %d", n), [n](long, unsigned via) { Wheel(n, via); });
    return true;
  }
  o.line(kUsage); return true;
}
