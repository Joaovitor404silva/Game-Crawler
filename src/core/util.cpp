#include "core_util.h"
#include <stdio.h>
#include <stdarg.h>
#include <wincrypt.h>
#pragma comment(lib, "advapi32.lib")

std::string Fmt(const char* fmt, ...) {
  char b[8192]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap); return b;
}
void Out::f(const char* fmt, ...) {
  char b[8192]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b) - 2, fmt, ap); va_end(ap);
  strcat_s(b, "\n"); emit(ctx, b);
}
std::vector<std::string> Tokenize(const std::string& s) {
  std::vector<std::string> r; size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && s[i] == ' ') i++;
    if (i >= s.size()) break;
    std::string t;
    if (s[i] == '"') { i++; while (i < s.size() && s[i] != '"') t += s[i++]; i++; }
    else while (i < s.size() && s[i] != ' ') t += s[i++];
    r.push_back(t);
  }
  return r;
}
bool ParseU64(const std::string& s0, uint64_t& v) {
  std::string s = s0; if (s.empty()) return false;
  int base = 10;
  if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s = s.substr(2); base = 16; }
  else if (s.find_first_of("abcdefABCDEF") != std::string::npos) base = 16;
  char* e; v = _strtoui64(s.c_str(), &e, base); return *e == 0;
}
std::string Lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
std::string Narrow(const wchar_t* w, int n) {
  if (!w) return ""; if (n < 0) n = (int)wcslen(w);
  int k = WideCharToMultiByte(CP_UTF8, 0, w, n, nullptr, 0, nullptr, nullptr);
  std::string r(k, 0); WideCharToMultiByte(CP_UTF8, 0, w, n, &r[0], k, nullptr, nullptr); return r;
}
std::wstring Widen(const std::string& s) {
  int k = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
  std::wstring r(k, 0); MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &r[0], k); return r;
}

std::string Md5Bytes(const void* data, size_t n) {
  HCRYPTPROV prov = 0; HCRYPTHASH h = 0; std::string out;
  if (!CryptAcquireContextA(&prov, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) return "";
  if (CryptCreateHash(prov, CALG_MD5, 0, 0, &h)) {
    const BYTE* p = (const BYTE*)data;
    bool ok = true;
    while (n && ok) { DWORD k = (DWORD)(n > (1u << 24) ? (1u << 24) : n); ok = CryptHashData(h, p, k, 0) != 0; p += k; n -= k; }
    BYTE d[16]; DWORD dl = 16;
    if (ok && CryptGetHashParam(h, HP_HASHVAL, d, &dl, 0)) { char b[40]; for (int i = 0; i < 16; i++) sprintf_s(b + i * 2, 3, "%02x", d[i]); out = b; }
    CryptDestroyHash(h);
  }
  CryptReleaseContext(prov, 0); return out;
}
std::string FileMd5(const std::string& path) {
  FILE* f; if (fopen_s(&f, path.c_str(), "rb")) return "";
  std::string d; char buf[65536]; size_t k;
  while ((k = fread(buf, 1, sizeof buf, f)) > 0) d.append(buf, k);
  fclose(f); return Md5Bytes(d.data(), d.size());
}
