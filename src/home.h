#pragma once
#include <windows.h>
#include <string>

inline std::string S2Env(const char* key, const std::string& def) {
  char b[MAX_PATH];
  DWORD n = GetEnvironmentVariableA(key, b, sizeof b);
  return n && n < sizeof b ? std::string(b) : def;
}
inline const std::string& S2Home() { static std::string h = S2Env("ANOMALY_HOME", "C:\\s2dev"); return h; }
inline std::string S2Path(const char* rel) { return S2Home() + "\\" + rel; }
