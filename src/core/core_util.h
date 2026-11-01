#pragma once
#include <windows.h>
#include <stdint.h>
#include <string>
#include <vector>
#include "api.h"

extern HostApi* H;
#define LOG(...) CoreLog(__VA_ARGS__)

bool SafeRead(void* dst, uintptr_t src, size_t n);
bool SafeWrite(uintptr_t dst, const void* src, size_t n);
template <class T> T Rd(uintptr_t a, T def = T()) { T v; return SafeRead(&v, a, sizeof(T)) ? v : def; }
inline uintptr_t RdP(uintptr_t a) { return Rd<uintptr_t>(a, 0); }

struct Out {
  EmitFn emit; void* ctx;
  void line(const std::string& s) { emit(ctx, (s + "\n").c_str()); }
  void f(const char* fmt, ...);
};
std::string Fmt(const char* fmt, ...);
std::vector<std::string> Tokenize(const std::string& s);
bool ParseU64(const std::string& s, uint64_t& v);
std::string Lower(std::string s);
std::string Narrow(const wchar_t* w, int n = -1);
std::wstring Widen(const std::string& s);
std::string Md5Bytes(const void* data, size_t n);
std::string FileMd5(const std::string& path);
void CoreLog(const char* fmt, ...);
void CoreLogClose();
std::string PortLogPath();
std::string LogDir();
