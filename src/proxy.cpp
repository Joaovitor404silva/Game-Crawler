#include <windows.h>
#include <string>
#pragma comment(linker, "/export:GetFileVersionInfoA=C:\\Windows\\System32\\version.GetFileVersionInfoA")
#pragma comment(linker, "/export:GetFileVersionInfoByHandle=C:\\Windows\\System32\\version.GetFileVersionInfoByHandle")
#pragma comment(linker, "/export:GetFileVersionInfoExA=C:\\Windows\\System32\\version.GetFileVersionInfoExA")
#pragma comment(linker, "/export:GetFileVersionInfoExW=C:\\Windows\\System32\\version.GetFileVersionInfoExW")
#pragma comment(linker, "/export:GetFileVersionInfoSizeA=C:\\Windows\\System32\\version.GetFileVersionInfoSizeA")
#pragma comment(linker, "/export:GetFileVersionInfoSizeExA=C:\\Windows\\System32\\version.GetFileVersionInfoSizeExA")
#pragma comment(linker, "/export:GetFileVersionInfoSizeExW=C:\\Windows\\System32\\version.GetFileVersionInfoSizeExW")
#pragma comment(linker, "/export:GetFileVersionInfoSizeW=C:\\Windows\\System32\\version.GetFileVersionInfoSizeW")
#pragma comment(linker, "/export:GetFileVersionInfoW=C:\\Windows\\System32\\version.GetFileVersionInfoW")
#pragma comment(linker, "/export:VerFindFileA=C:\\Windows\\System32\\version.VerFindFileA")
#pragma comment(linker, "/export:VerFindFileW=C:\\Windows\\System32\\version.VerFindFileW")
#pragma comment(linker, "/export:VerInstallFileA=C:\\Windows\\System32\\version.VerInstallFileA")
#pragma comment(linker, "/export:VerInstallFileW=C:\\Windows\\System32\\version.VerInstallFileW")
#pragma comment(linker, "/export:VerLanguageNameA=C:\\Windows\\System32\\version.VerLanguageNameA")
#pragma comment(linker, "/export:VerLanguageNameW=C:\\Windows\\System32\\version.VerLanguageNameW")
#pragma comment(linker, "/export:VerQueryValueA=C:\\Windows\\System32\\version.VerQueryValueA")
#pragma comment(linker, "/export:VerQueryValueW=C:\\Windows\\System32\\version.VerQueryValueW")

static std::string HomeFor(HMODULE m) {
  char e[MAX_PATH]; DWORD n = GetEnvironmentVariableA("UESCAN_HOME", e, MAX_PATH); if (n && n < MAX_PATH) return e;
  char p[MAX_PATH]; n = GetModuleFileNameA(m, p, MAX_PATH); if (!n || n >= MAX_PATH) return "";
  char* s = strrchr(p, '\\'); if (s) *s = 0;
  std::string dir = p, cfg = dir + "\\uescan_home.txt";
  HANDLE f = CreateFileA(cfg.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return dir;
  char b[MAX_PATH] = {}; DWORD r = 0; ReadFile(f, b, MAX_PATH - 1, &r, nullptr); CloseHandle(f);
  std::string h(b, r); size_t k = h.find_first_of("\r\n"); if (k != std::string::npos) h.resize(k);
  while (!h.empty() && (h.back() == ' ' || h.back() == '\\')) h.pop_back();
  return h.empty() ? dir : h;
}

static DWORD WINAPI Boot(LPVOID m) {
  std::string home = HomeFor((HMODULE)m); if (home.empty()) return 0;
  SetEnvironmentVariableA("UESCAN_HOME", home.c_str());
  LoadLibraryA((home + "\\bin\\UEScanner.dll").c_str());
  return 0;
}

BOOL APIENTRY DllMain(HMODULE m, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(m); HANDLE h = CreateThread(nullptr, 0, Boot, m, 0, nullptr); if (h) CloseHandle(h); }
  return TRUE;
}
