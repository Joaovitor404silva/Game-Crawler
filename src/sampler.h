#pragma once
#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")

namespace smp {

enum Type { F32, F64, I8, U8, I16, U16, I32, U32, I64, U64, HEX, BOOLT, TEXT };

struct Col {
  std::string name, expr, obj, path;
  Type type = F32;
  bool isProp = false;
};

inline bool ParseType(const std::string& s, Type& t) {
  static const struct { const char* n; Type t; } tab[] = { { "f32", F32 }, { "f64", F64 }, { "i8", I8 }, { "u8", U8 }, { "i16", I16 }, { "u16", U16 },
    { "i32", I32 }, { "u32", U32 }, { "i64", I64 }, { "u64", U64 }, { "hex", HEX }, { "ptr", HEX }, { "bool", BOOLT } };
  for (auto& e : tab) if (s == e.n) { t = e.t; return true; }
  return false;
}
inline int TypeSize(Type t) {
  switch (t) { case I8: case U8: case BOOLT: return 1; case I16: case U16: return 2; case F32: case I32: case U32: return 4; default: return 8; }
}

inline bool ParseCols(const std::string& spec, std::vector<Col>& out, std::string& err) {
  size_t s = 0; int idx = 0;
  while (s <= spec.size()) {
    size_t e = spec.find(',', s); std::string item = spec.substr(s, e == std::string::npos ? std::string::npos : e - s);
    s = e == std::string::npos ? spec.size() + 1 : e + 1;
    if (item.empty()) continue;
    Col c; c.name = "c" + std::to_string(idx++);
    size_t eq = item.find('=');
    if (eq != std::string::npos && eq > 0 && item.find_first_of("[]+") > eq) { c.name = item.substr(0, eq); item = item.substr(eq + 1); }
    if (!item.empty() && item[0] == '@') {
      size_t bar = item.find('|');
      if (bar == std::string::npos) { err = "column '" + c.name + "': property form is @obj|Prop.Path"; return false; }
      c.isProp = true; c.obj = item.substr(1, bar - 1); c.path = item.substr(bar + 1); c.type = TEXT;
    } else {
      size_t colon = item.rfind(':');
      c.expr = item;
      if (colon != std::string::npos) {
        Type t; std::string ts = item.substr(colon + 1);
        if (ParseType(ts, t)) { c.type = t; c.expr = item.substr(0, colon); }
        else { err = "column '" + c.name + "': unknown type '" + ts + "' (f32 f64 i8 u8 i16 u16 i32 u32 i64 u64 hex bool)"; return false; }
      }
      if (c.expr.empty()) { err = "column '" + c.name + "': empty expression"; return false; }
    }
    out.push_back(c);
  }
  if (out.empty()) { err = "no columns"; return false; }
  return true;
}

inline std::string FormatValue(Type t, const uint8_t* b) {
  char s[48];
  switch (t) {
    case F32: { float v; memcpy(&v, b, 4); snprintf(s, sizeof s, "%.6g", v); break; }
    case F64: { double v; memcpy(&v, b, 8); snprintf(s, sizeof s, "%.10g", v); break; }
    case I8: snprintf(s, sizeof s, "%d", (int)(int8_t)b[0]); break;
    case U8: snprintf(s, sizeof s, "%u", (unsigned)b[0]); break;
    case BOOLT: snprintf(s, sizeof s, "%d", b[0] ? 1 : 0); break;
    case I16: { int16_t v; memcpy(&v, b, 2); snprintf(s, sizeof s, "%d", (int)v); break; }
    case U16: { uint16_t v; memcpy(&v, b, 2); snprintf(s, sizeof s, "%u", (unsigned)v); break; }
    case I32: { int32_t v; memcpy(&v, b, 4); snprintf(s, sizeof s, "%d", v); break; }
    case U32: { uint32_t v; memcpy(&v, b, 4); snprintf(s, sizeof s, "%u", v); break; }
    case I64: { int64_t v; memcpy(&v, b, 8); snprintf(s, sizeof s, "%lld", (long long)v); break; }
    case U64: { uint64_t v; memcpy(&v, b, 8); snprintf(s, sizeof s, "%llu", (unsigned long long)v); break; }
    default: { uint64_t v; memcpy(&v, b, 8); snprintf(s, sizeof s, "0x%llx", (unsigned long long)v); break; }
  }
  return s;
}

inline std::string Header(const std::vector<Col>& cols) {
  std::string h = "tick\tms";
  for (auto& c : cols) h += "\t" + c.name;
  return h;
}

typedef std::function<bool(const Col&, std::string&)> EvalFn;
typedef std::function<uint64_t()> TickFn;

struct Run {
  std::string id, file, err;
  std::vector<Col> cols;
  int hz = 10, durMs = 1000;
  std::atomic<bool> stop{ false }, done{ false };
  std::atomic<long> rows{ 0 };
  HANDLE th = nullptr;
  EvalFn eval; TickFn tick;
};

inline void Loop(Run* r) {
  FILE* f = nullptr;
  if (fopen_s(&f, r->file.c_str(), "wb")) { r->err = "cannot open " + r->file; r->done = true; return; }
  std::string h = Header(r->cols) + "\n"; fwrite(h.data(), 1, h.size(), f); fflush(f);
  LARGE_INTEGER fq, t0, now; QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&t0);
  double period = 1000.0 / (r->hz > 0 ? r->hz : 1);
  double next = 0;
  while (!r->stop) {
    QueryPerformanceCounter(&now);
    double ms = (double)(now.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart;
    if (ms > r->durMs) break;
    if (ms + 0.5 >= next) {
      std::string row = std::to_string((unsigned long long)(r->tick ? r->tick() : 0)) + "\t" + std::to_string((long long)ms);
      for (auto& c : r->cols) { std::string v; if (!r->eval(c, v)) v = "NA"; row += "\t" + v; }
      row += "\n"; fwrite(row.data(), 1, row.size(), f); r->rows++;
      next += period; if (next < ms - period * 4) next = ms + period;
      if (r->rows % 20 == 0) fflush(f);
    } else {
      double wait = next - ms; if (wait > 1.5) Sleep((DWORD)(wait - 1)); else Sleep(0);
    }
  }
  fclose(f); r->done = true;
}
inline DWORD WINAPI Thunk(LPVOID p) { timeBeginPeriod(1); Loop((Run*)p); timeEndPeriod(1); return 0; }

}
