#pragma once
#include <string>
#include <vector>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

namespace scn {

enum Type { I8, I16, I32, I64, F32, F64, STR, BAD };

inline Type ParseType(const std::string& s) {
  if (s == "i8" || s == "u8" || s == "byte") return I8;
  if (s == "i16" || s == "u16") return I16;
  if (s == "i32" || s == "u32" || s == "int") return I32;
  if (s == "i64" || s == "u64") return I64;
  if (s == "f32" || s == "float") return F32;
  if (s == "f64" || s == "double") return F64;
  if (s == "str" || s == "string") return STR;
  return BAD;
}
inline const char* TypeName(Type t) { static const char* n[] = { "i8", "i16", "i32", "i64", "f32", "f64", "str", "?" }; return n[t]; }
inline size_t Size(Type t) { static const size_t z[] = { 1, 2, 4, 8, 4, 8, 0, 0 }; return z[t]; }
inline size_t Align(Type t) { return t == STR ? 1 : Size(t); }

struct Val { double d = 0; long long i = 0; };

inline Val Load(Type t, const uint8_t* p) {
  Val v;
  switch (t) {
  case I8: v.i = *(const int8_t*)p; break;
  case I16: v.i = *(const int16_t*)p; break;
  case I32: v.i = *(const int32_t*)p; break;
  case I64: v.i = *(const int64_t*)p; break;
  case F32: v.d = *(const float*)p; break;
  case F64: v.d = *(const double*)p; break;
  default: break;
  }
  if (t != F32 && t != F64) v.d = (double)v.i;
  return v;
}

inline bool ParseVal(Type t, const std::string& s, Val& v) {
  if (s.empty()) return false;
  char* e = nullptr;
  if (t == F32 || t == F64) { v.d = strtod(s.c_str(), &e); return *e == 0; }
  v.i = strtoll(s.c_str(), &e, 0); v.d = (double)v.i; return *e == 0;
}

inline bool Store(Type t, const Val& v, uint8_t* out) {
  switch (t) {
  case I8: { int8_t x = (int8_t)v.i; memcpy(out, &x, 1); return true; }
  case I16: { int16_t x = (int16_t)v.i; memcpy(out, &x, 2); return true; }
  case I32: { int32_t x = (int32_t)v.i; memcpy(out, &x, 4); return true; }
  case I64: memcpy(out, &v.i, 8); return true;
  case F32: { float x = (float)v.d; memcpy(out, &x, 4); return true; }
  case F64: memcpy(out, &v.d, 8); return true;
  default: return false;
  }
}

inline bool Eq(Type t, const Val& a, const Val& b, double tol) {
  if (t == F32 || t == F64) return fabs(a.d - b.d) <= tol;
  return a.i == b.i;
}

enum Op { EQ, CHANGED, UNCHANGED, INC, DEC, OPBAD };
inline Op ParseOp(const std::string& s) {
  if (s == "eq" || s == "=") return EQ;
  if (s == "changed" || s == "ch") return CHANGED;
  if (s == "unchanged" || s == "same") return UNCHANGED;
  if (s == "inc" || s == "up" || s == "+") return INC;
  if (s == "dec" || s == "down" || s == "-") return DEC;
  return OPBAD;
}

inline bool Keep(Type t, Op op, const Val& old, const Val& cur, const Val& want, double tol) {
  switch (op) {
  case EQ: return Eq(t, cur, want, tol);
  case CHANGED: return !Eq(t, cur, old, 0);
  case UNCHANGED: return Eq(t, cur, old, 0);
  case INC: return cur.d > old.d;
  case DEC: return cur.d < old.d;
  default: return false;
  }
}

struct Hit { uintptr_t addr; Val last; };

inline void ScanBlock(Type t, const uint8_t* buf, size_t len, uintptr_t base, const Val& want, double tol, const std::string& str, std::vector<Hit>& out, size_t cap) {
  if (t == STR) {
    if (str.empty() || len < str.size()) return;
    for (size_t i = 0; i + str.size() <= len && out.size() < cap; i++) {
      const void* h = memchr(buf + i, str[0], len - str.size() + 1 - i); if (!h) return;
      i = (const uint8_t*)h - buf; if (!memcmp(buf + i, str.data(), str.size())) out.push_back({ base + i, Val() });
    }
    return;
  }
  size_t sz = Size(t), al = Align(t);
  for (size_t i = (al - (base % al)) % al; i + sz <= len && out.size() < cap; i += al) {
    Val v = Load(t, buf + i);
    if (Eq(t, v, want, tol)) out.push_back({ base + i, v });
  }
}

inline std::string Fmt(Type t, const Val& v) {
  char b[64];
  if (t == F32 || t == F64) snprintf(b, sizeof b, "%g", v.d); else snprintf(b, sizeof b, "%lld", v.i);
  return b;
}

}
