#pragma once
#include <windows.h>
#include <string>

inline std::string UEnv(const char* key, const std::string& def) {
  char b[MAX_PATH];
  DWORD n = GetEnvironmentVariableA(key, b, sizeof b);
  return n && n < sizeof b ? std::string(b) : def;
}
inline const std::string& UEHome() { static std::string h = UEnv("CRAWLER_HOME", "C:\\s2dev"); return h; }
inline std::string UEPath(const char* rel) { return UEHome() + "\\" + rel; }
