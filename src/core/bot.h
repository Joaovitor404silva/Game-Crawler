#pragma once
#include <windows.h>
#include <string>
#include <stdint.h>
#include <stdlib.h>

namespace bot {

inline int ParseKey(std::string s) {
  for (auto& c : s) c = (char)tolower((unsigned char)c);
  if (s.size() == 1) {
    char c = s[0];
    if (c >= 'a' && c <= 'z') return 'A' + (c - 'a');
    if (c >= '0' && c <= '9') return c;
    if (c == '`' || c == '~') return VK_OEM_3;
    if (c == '-') return VK_OEM_MINUS;
    if (c == '=') return VK_OEM_PLUS;
    if (c == '[') return VK_OEM_4;
    if (c == ']') return VK_OEM_6;
    if (c == ';') return VK_OEM_1;
    if (c == '\'') return VK_OEM_7;
    if (c == ',') return VK_OEM_COMMA;
    if (c == '.') return VK_OEM_PERIOD;
    if (c == '/') return VK_OEM_2;
    if (c == '\\') return VK_OEM_5;
  }
  if (s.size() >= 2 && s[0] == 'f' && s[1] >= '1' && s[1] <= '9') { int n = atoi(s.c_str() + 1); if (n >= 1 && n <= 24) return VK_F1 + n - 1; }
  if (s.rfind("0x", 0) == 0) { int v = (int)strtol(s.c_str(), nullptr, 16); return v > 0 && v < 256 ? v : 0; }
  if (s.rfind("vk", 0) == 0 && s.size() > 2) { int v = atoi(s.c_str() + 2); return v > 0 && v < 256 ? v : 0; }
  if (s.rfind("num", 0) == 0 && s.size() == 4 && s[3] >= '0' && s[3] <= '9') return VK_NUMPAD0 + (s[3] - '0');
  static const struct { const char* n; int vk; } tab[] = {
    { "space", VK_SPACE }, { "enter", VK_RETURN }, { "return", VK_RETURN }, { "esc", VK_ESCAPE }, { "escape", VK_ESCAPE },
    { "tab", VK_TAB }, { "backspace", VK_BACK }, { "bs", VK_BACK }, { "shift", VK_LSHIFT }, { "lshift", VK_LSHIFT }, { "rshift", VK_RSHIFT },
    { "ctrl", VK_LCONTROL }, { "control", VK_LCONTROL }, { "lctrl", VK_LCONTROL }, { "rctrl", VK_RCONTROL },
    { "alt", VK_LMENU }, { "lalt", VK_LMENU }, { "ralt", VK_RMENU }, { "up", VK_UP }, { "down", VK_DOWN }, { "left", VK_LEFT }, { "right", VK_RIGHT },
    { "insert", VK_INSERT }, { "ins", VK_INSERT }, { "delete", VK_DELETE }, { "del", VK_DELETE }, { "home", VK_HOME }, { "end", VK_END },
    { "pageup", VK_PRIOR }, { "pgup", VK_PRIOR }, { "pagedown", VK_NEXT }, { "pgdn", VK_NEXT }, { "caps", VK_CAPITAL }, { "capslock", VK_CAPITAL },
    { "tilde", VK_OEM_3 }, { "grave", VK_OEM_3 }, { "console", VK_OEM_3 }, { "lwin", VK_LWIN }, { "apps", VK_APPS }, { "pause", VK_PAUSE },
    { "lmb", VK_LBUTTON }, { "rmb", VK_RBUTTON }, { "mmb", VK_MBUTTON }, { "mouse4", VK_XBUTTON1 }, { "mouse5", VK_XBUTTON2 },
  };
  for (auto& e : tab) if (s == e.n) return e.vk;
  return 0;
}

inline bool IsExtended(int vk) {
  switch (vk) {
  case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
  case VK_PRIOR: case VK_NEXT: case VK_RCONTROL: case VK_RMENU: case VK_LWIN: case VK_RWIN: case VK_APPS: case VK_DIVIDE: return true;
  default: return false;
  }
}

inline int DikOf(int scan, int vk) { return (scan & 0x7f) | (IsExtended(vk) ? 0x80 : 0); }

inline LPARAM KeyLParam(int scan, int vk, bool down) {
  LPARAM l = 1 | ((LPARAM)(scan & 0xff) << 16);
  if (IsExtended(vk)) l |= (LPARAM)1 << 24;
  if (!down) l |= ((LPARAM)1 << 30) | ((LPARAM)1 << 31);
  return l;
}

inline int ButtonIndex(const std::string& s) {
  if (s == "left" || s == "l" || s == "lmb" || s == "1") return 0;
  if (s == "right" || s == "r" || s == "rmb" || s == "2") return 1;
  if (s == "middle" || s == "m" || s == "mmb" || s == "3") return 2;
  if (s == "x1" || s == "mouse4" || s == "4") return 3;
  if (s == "x2" || s == "mouse5" || s == "5") return 4;
  return -1;
}

enum Via : unsigned { MSG = 1, POLL = 2, RAW = 4, DI = 8, ALL = 15 };

inline unsigned ParseVia(const std::string& s) {
  unsigned v = 0; size_t i = 0;
  while (i <= s.size()) {
    size_t j = s.find(',', i); if (j == std::string::npos) j = s.size();
    std::string p = s.substr(i, j - i);
    if (p == "msg") v |= MSG; else if (p == "poll") v |= POLL; else if (p == "raw") v |= RAW; else if (p == "di") v |= DI; else if (p == "all") v |= ALL; else return 0;
    i = j + 1;
  }
  return v;
}

}
