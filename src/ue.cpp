
#include "core.h"
#include "home.h"
#include "json_out.h"
#include "sig.h"
#include "ue_opts.h"
#include "pose.h"
#include "quest.h"
#include "containers.h"
#include "retry.h"
#include <unordered_map>
#include <map>
#include <mutex>
#include <set>
#include <cstdio>
#include <algorithm>

static uintptr_t g_fnToString, g_fnCtor;
static const int PE_VSLOT = 0x278;

static int O_INDEX = 0xC, O_CLASS = 0x10, O_NAME = 0x18, O_OUTER = 0x20, F_NEXT = 0x28;
static int S_SUPER = 0x40, S_CHILDREN = 0x48, S_CHILDPROPS = 0x50, S_SIZE = 0x58;
static int FF_CLASS = 0x8, FF_NEXT = 0x20, FF_NAME = 0x28, P_ARRAYDIM = 0x34, P_ELEMSIZE = 0x38, P_FLAGS = 0x40, P_OFFSET = 0x4C, P_SIZE = 0x78;
static int ITEM_STRIDE = 24;
static bool g_ok;
static uintptr_t g_arr;
static bool EnsureUObj(std::string& why);

struct FStr { wchar_t* data; int num, max; };

static std::mutex g_nm;
static std::unordered_map<uint32_t, std::string> g_names;
std::string NameStr(uint64_t fname) {
  uint32_t idx = (uint32_t)fname, num = (uint32_t)(fname >> 32);
  std::string s;
  { std::lock_guard<std::mutex> l(g_nm); auto it = g_names.find(idx); if (it != g_names.end()) s = it->second; }
  if (s.empty()) {
    uint64_t n0 = idx; FStr out{};
    if (!g_fnToString || !SafeNameToString((void*)g_fnToString, &n0, &out) || !out.data || out.num <= 0 || out.num > 4096) return Fmt("<name %u>", idx);
    s = Narrow(out.data, out.num - 1);
    std::lock_guard<std::mutex> l(g_nm); g_names[idx] = s;
  }
  if (num) s += Fmt("_%u", num - 1);
  return s;
}
static bool MakeFName(const std::string& s, uint64_t& out) {
  typedef void(__fastcall* Ctor)(uint64_t*, const wchar_t*, int);
  std::wstring w = Widen(s); out = 0;
  struct L { static bool Call(Ctor f, uint64_t* o, const wchar_t* w) { __try { f(o, w, 1); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } } };
  if (!g_fnCtor) return false;
  return L::Call((Ctor)g_fnCtor, &out, w.c_str());
}

static int NumObjects() { return Rd<int>(g_arr + 0x10 + 0x14, 0); }
static uintptr_t ObjAt(int i) {
  uintptr_t chunks = RdP(g_arr + 0x10); if (!chunks || i < 0) return 0;
  uintptr_t ch = RdP(chunks + (uintptr_t)(i / 65536) * 8); if (!ch) return 0;
  return RdP(ch + (uintptr_t)(i % 65536) * ITEM_STRIDE);
}
bool IsObj(uintptr_t p) {
  if (!g_ok || p < 0x10000 || (p & 7)) return false;
  int idx = Rd<int>(p + O_INDEX, -1); if (idx < 0 || idx >= NumObjects()) return false;
  return ObjAt(idx) == p;
}
static uintptr_t ClassOf(uintptr_t o) { return RdP(o + O_CLASS); }
static uintptr_t OuterOf(uintptr_t o) { return RdP(o + O_OUTER); }
std::string ObjName(uintptr_t o) { return NameStr(Rd<uint64_t>(o + O_NAME)); }
static std::string ClassName(uintptr_t o) { uintptr_t c = ClassOf(o); return c ? ObjName(c) : "?"; }
static std::string PathOf(uintptr_t o) {
  std::vector<uintptr_t> chain; for (uintptr_t p = o; p && chain.size() < 32; p = OuterOf(p)) chain.push_back(p);
  std::string s;
  for (size_t i = chain.size(); i-- > 0;) {
    if (i + 1 < chain.size()) {
      uintptr_t outer = chain[i + 1];
      bool sub = OuterOf(outer) && ClassName(outer) != "Package" && ClassName(OuterOf(outer)) == "Package";
      s += sub ? ":" : ".";
    }
    s += ObjName(chain[i]);
  }
  return s;
}
std::string DescribeObj(uintptr_t a) { if (!IsObj(a)) return ""; return ClassName(a) + " " + PathOf(a); }
static uintptr_t SuperOf(uintptr_t s) { return RdP(s + S_SUPER); }
static bool IsA(uintptr_t o, const std::string& cls) {
  for (uintptr_t c = ClassOf(o); c; c = SuperOf(c)) if (ObjName(c) == cls) return true; return false;
}
static bool IsDefault(uintptr_t o) { return ObjName(o).rfind("Default__", 0) == 0; }

static uintptr_t FindFirst(std::function<bool(uintptr_t)> pred) {
  int n = NumObjects();
  for (int i = 0; i < n; i++) { uintptr_t o = ObjAt(i); if (o && pred(o)) return o; }
  return 0;
}

static std::string PropType(uintptr_t p) { uintptr_t fc = RdP(p + FF_CLASS); return fc ? NameStr(Rd<uint64_t>(fc)) : "?"; }
static std::string PropName(uintptr_t p) { return NameStr(Rd<uint64_t>(p + FF_NAME)); }
static int PropOff(uintptr_t p) { return Rd<int>(p + P_OFFSET); }
static int PropElem(uintptr_t p) { return Rd<int>(p + P_ELEMSIZE); }
static int PropDim(uintptr_t p) { return Rd<int>(p + P_ARRAYDIM); }
static uint64_t PropFlags(uintptr_t p) { return Rd<uint64_t>(p + P_FLAGS); }
static uintptr_t PropX(uintptr_t p, int k = 0) { return RdP(p + P_SIZE + k * 8); }
static uintptr_t PropInner(uintptr_t p) { return PropX(p, cnt::InnerSlot(PropType(p).c_str())); }

static void ForProps(uintptr_t st, bool supers, std::function<bool(uintptr_t)> fn) {
  for (uintptr_t s = st; s; s = supers ? SuperOf(s) : 0) {
    int guard = 0;
    for (uintptr_t p = RdP(s + S_CHILDPROPS); p && guard < 5000; p = RdP(p + FF_NEXT), guard++) if (!fn(p)) return;
  }
}
static uintptr_t FindProp(uintptr_t st, const std::string& name) {
  uintptr_t r = 0; std::string ln = Lower(name);
  ForProps(st, true, [&](uintptr_t p) { if (Lower(PropName(p)) == ln) { r = p; return false; } return true; });
  return r;
}
static uintptr_t FindFunc(uintptr_t cls, const std::string& name) {
  std::string ln = Lower(name);
  for (uintptr_t s = cls; s; s = SuperOf(s))
    for (uintptr_t f = RdP(s + S_CHILDREN); f; f = RdP(f + F_NEXT)) if (Lower(ObjName(f)) == ln) return f;
  return 0;
}

static std::string FStrAt(uintptr_t a) {
  FStr s; if (!SafeRead(&s, a, sizeof(s))) return "<bad>";
  if (!s.data || s.num <= 0) return "\"\"";
  if (s.num > 2048) s.num = 2048;
  std::vector<wchar_t> w(s.num); if (!SafeRead(w.data(), (uintptr_t)s.data, s.num * 2)) return "<bad str>";
  return "\"" + Narrow(w.data(), s.num - 1) + "\"";
}
static std::string EnumName(uintptr_t en, int64_t v) {
  uintptr_t data = RdP(en + 0x40); int num = Rd<int>(en + 0x48);
  if (!data || num < 0 || num > 4096) return "";
  for (int i = 0; i < num; i++) if (Rd<int64_t>(data + i * 16 + 8) == v) { std::string n = NameStr(Rd<uint64_t>(data + i * 16)); size_t k = n.rfind("::"); return k == std::string::npos ? n : n.substr(k + 2); }
  return "";
}

static std::string ValueStr(uintptr_t p, uintptr_t a, int depth, Out* o, const std::string& indent);
static std::string StructStr(uintptr_t st, uintptr_t a, int depth, Out* o, const std::string& indent) {
  std::string tn = ObjName(st);
  if (depth <= 0 || !o) {
    std::string s = tn + "{"; int k = 0; bool simple = true;
    ForProps(st, true, [&](uintptr_t q) { std::string ty = PropType(q); if (ty != "FloatProperty" && ty != "DoubleProperty" && ty != "IntProperty" && ty != "BoolProperty" && ty != "ByteProperty") { simple = false; return false; } if (k++ < 8) s += (k > 1 ? " " : "") + PropName(q) + "=" + ValueStr(q, a + PropOff(q), 0, nullptr, ""); return true; });
    return simple ? s + "}" : tn + "{...}";
  }
  ForProps(st, true, [&](uintptr_t q) {
    std::string v = ValueStr(q, a + PropOff(q), 0, nullptr, "");
    o->f("%s    +0x%04x %-28s %-16s %s", indent.c_str(), PropOff(q), PropName(q).c_str(), PropType(q).c_str(), v.c_str());
    if (depth > 1 && v.size() > 5 && v.compare(v.size() - 5, 5, "{...}") == 0) StructStr(PropX(q), a + PropOff(q), depth - 1, o, indent + "    ");
    return true;
  });
  return "";
}
static std::string ValueStr(uintptr_t p, uintptr_t a, int depth, Out* o, const std::string& indent) {
  std::string ty = PropType(p);
  if (ty == "BoolProperty") { uint8_t off = Rd<uint8_t>(p + P_SIZE + 1), mask = Rd<uint8_t>(p + P_SIZE + 3); return (Rd<uint8_t>(a + off) & mask) ? "true" : "false"; }
  if (ty == "FloatProperty") return Fmt("%g", Rd<float>(a));
  if (ty == "DoubleProperty") return Fmt("%.6g", Rd<double>(a));
  if (ty == "IntProperty") return Fmt("%d", Rd<int>(a));
  if (ty == "Int64Property") return Fmt("%lld", Rd<int64_t>(a));
  if (ty == "UInt32Property") return Fmt("%u", Rd<uint32_t>(a));
  if (ty == "UInt64Property") return Fmt("%llu", Rd<uint64_t>(a));
  if (ty == "Int16Property") return Fmt("%d", Rd<int16_t>(a));
  if (ty == "UInt16Property") return Fmt("%u", Rd<uint16_t>(a));
  if (ty == "Int8Property") return Fmt("%d", Rd<int8_t>(a));
  if (ty == "ByteProperty") { uint8_t v = Rd<uint8_t>(a); uintptr_t en = PropX(p); std::string n = en ? EnumName(en, v) : ""; return n.empty() ? Fmt("%u", v) : Fmt("%u (%s)", v, n.c_str()); }
  if (ty == "EnumProperty") { int sz = PropElem(p); int64_t v = sz == 1 ? Rd<uint8_t>(a) : sz == 2 ? Rd<uint16_t>(a) : sz == 4 ? Rd<int>(a) : Rd<int64_t>(a); std::string n = EnumName(PropX(p, 1), v); return n.empty() ? Fmt("%lld", v) : Fmt("%lld (%s)", v, n.c_str()); }
  if (ty == "NameProperty") return NameStr(Rd<uint64_t>(a));
  if (ty == "StrProperty") return FStrAt(a);
  if (ty == "ObjectProperty" || ty == "ClassProperty" || ty == "InterfaceProperty") { uintptr_t v = RdP(a); if (!v) return "null"; std::string d = IsObj(v) ? ClassName(v) + " " + ObjName(v) : "<not obj>"; return Fmt("0x%llx %s", v, d.c_str()); }
  if (ty == "WeakObjectProperty") return Fmt("weak idx=%d serial=%d", Rd<int>(a), Rd<int>(a + 4));
  if (ty == "SoftObjectProperty" || ty == "SoftClassProperty") return "soft";
  if (ty == "ArrayProperty") { uintptr_t d = RdP(a); int n = Rd<int>(a + 8); return Fmt("[%d] data=0x%llx of %s", n, d, PropType(PropInner(p)).c_str()); }
  if (ty == "MapProperty" || ty == "SetProperty") return Fmt("%s num=%d", ty.c_str(), Rd<int>(a + 8));
  if (ty == "StructProperty") return StructStr(PropX(p), a, depth, o, indent);
  if (ty == "TextProperty") return "text";
  if (ty.find("Delegate") != std::string::npos) return "delegate";
  return Fmt("? (%s, %d bytes)", ty.c_str(), PropElem(p));
}

static bool PropPath(uintptr_t obj, const std::string& path, uintptr_t& prop, uintptr_t& addr, std::string& err) {
  uintptr_t st = ClassOf(obj), base = obj; size_t i = 0;
  while (i <= path.size()) {
    size_t j = path.find('.', i); std::string seg = path.substr(i, j == std::string::npos ? std::string::npos : j - i);
    int idx = -1; size_t br = seg.find('['); if (br != std::string::npos) { idx = atoi(seg.c_str() + br + 1); seg = seg.substr(0, br); }
    uintptr_t p = FindProp(st, seg); if (!p) { err = "no property '" + seg + "' in " + ObjName(st); return false; }
    uintptr_t a = base + PropOff(p);
    if (idx >= 0) {
      if (PropType(p) == "ArrayProperty") { int n = Rd<int>(a + 8); if (idx >= n) { err = Fmt("index %d >= %d", idx, n); return false; } uintptr_t in = PropInner(p); a = RdP(a) + (uintptr_t)idx * PropElem(in); p = in; }
      else a += (uintptr_t)idx * PropElem(p);
    }
    if (j == std::string::npos) { prop = p; addr = a; return true; }
    std::string ty = PropType(p);
    if (ty == "StructProperty") { st = PropX(p); base = a; }
    else if (ty == "ObjectProperty") { base = RdP(a); if (!IsObj(base)) { err = seg + " is null/invalid"; return false; } st = ClassOf(base); }
    else { err = seg + " is " + ty + ", cannot descend"; return false; }
    i = j + 1;
  }
  err = "empty path"; return false;
}

static bool SetValue(uintptr_t p, uintptr_t a, const std::string& v, std::string& err, std::vector<std::wstring>* keep);
static bool GameFString(const std::string& v, FStr& out, std::string& err) {
  static uintptr_t ksl; if (!IsObj(ksl)) ksl = ResolveObj("Default__KismetStringLibrary");
  uintptr_t fn = ksl ? FindFunc(ClassOf(ksl), "Concat_StrStr") : 0; if (!fn) { err = "KismetStringLibrary::Concat_StrStr not found"; return false; }
  uint8_t buf[64] = {}; std::vector<std::wstring> keep; keep.reserve(4); uintptr_t ret = 0; int ai = 0;
  ForProps(fn, false, [&](uintptr_t q) { if (PropFlags(q) & 0x400) ret = q; else if (PropFlags(q) & 0x80) SetValue(q, (uintptr_t)buf + PropOff(q), ai++ == 0 ? v : "", err, &keep); return true; });
  if (!ret || PropOff(ret) + 16 > (int)sizeof(buf)) { err = "unexpected Concat_StrStr layout"; return false; }
  void* pe = (void*)RdP(RdP(ksl) + PE_VSLOT); int code = 0;
  if (!RunOnGame([&]() { code = SafeProcessEvent(pe, (void*)ksl, (void*)fn, buf); }, 5000, &err)) return false;
  if (code) { err = Fmt("ProcessEvent exception 0x%08x", code); return false; }
  memcpy(&out, buf + PropOff(ret), sizeof(FStr)); return true;
}
static bool SetValue(uintptr_t p, uintptr_t a, const std::string& v, std::string& err, std::vector<std::wstring>* keep) {
  std::string ty = PropType(p); bool ok = true;
  auto W = [&](const void* d, size_t n) { if (!SafeWrite(a, d, n)) { err = "write failed"; ok = false; } };
  if (ty == "BoolProperty") { uint8_t off = Rd<uint8_t>(p + P_SIZE + 1), mask = Rd<uint8_t>(p + P_SIZE + 3); uint8_t b = Rd<uint8_t>(a + off); bool on = v == "1" || Lower(v) == "true"; b = on ? (b | mask) : (b & ~mask); a += off; W(&b, 1); }
  else if (ty == "FloatProperty") { float f = (float)atof(v.c_str()); W(&f, 4); }
  else if (ty == "DoubleProperty") { double f = atof(v.c_str()); W(&f, 8); }
  else if (ty == "IntProperty" || ty == "UInt32Property" || ty == "Int16Property" || ty == "UInt16Property" || ty == "Int8Property" || ty == "ByteProperty" || ty == "Int64Property" || ty == "UInt64Property" || ty == "EnumProperty") {
    int64_t x = _strtoi64(v.c_str(), nullptr, 0); W(&x, PropElem(p));
  }
  else if (ty == "ObjectProperty" || ty == "ClassProperty") { uintptr_t x = 0; if (v != "null" && v != "0") { x = ResolveObj(v); if (!x) { err = "cannot resolve object " + v; return false; } } W(&x, 8); }
  else if (ty == "NameProperty") { uint64_t n; if (!MakeFName(v, n)) { err = "FName ctor failed"; return false; } W(&n, 8); }
  else if (ty == "StrProperty") {
    if (!keep) { FStr s; if (!GameFString(v, s, err)) return false; W(&s, sizeof(s)); return ok; }
    keep->push_back(Widen(v)); FStr s{ (wchar_t*)keep->back().c_str(), (int)keep->back().size() + 1, (int)keep->back().size() + 1 }; W(&s, sizeof(s));
  }
  else if (ty == "StructProperty") {
    std::vector<std::string> parts; size_t s = 0, e; std::string vv = v; while ((e = vv.find(',', s)) != std::string::npos) { parts.push_back(vv.substr(s, e - s)); s = e + 1; } parts.push_back(vv.substr(s));
    size_t k = 0; ForProps(PropX(p), true, [&](uintptr_t q) { if (k < parts.size()) ok = SetValue(q, a + PropOff(q), parts[k++], err, keep) && ok; return true; });
  }
  else if (ty == "ArrayProperty" && (v == "[]" || v.empty())) { uint8_t z[16] = {}; W(z, 16); }
  else { err = "unsupported type " + ty + (ty == "ArrayProperty" ? " (only [] = empty array)" : ""); return false; }
  return ok;
}

static bool ReadVec3(uintptr_t prop, uintptr_t a, double xyz[3]) {
  if (PropElem(prop) >= 24) { double d[3]; if (!SafeRead(d, a, sizeof d)) return false; memcpy(xyz, d, sizeof d); return true; }
  float f[3]; if (!SafeRead(f, a, sizeof f)) return false;
  for (int i = 0; i < 3; i++) xyz[i] = f[i];
  return true;
}
static bool ActorLoc(uintptr_t o, double xyz[3]) {
  uintptr_t p, a; std::string err;
  if (!PropPath(o, "RootComponent.RelativeLocation", p, a, err)) return false;
  return ReadVec3(p, a, xyz);
}
static int HiddenOf(uintptr_t o) {
  uintptr_t p, a; std::string err;
  if (!PropPath(o, "bHidden", p, a, err)) return -1;
  return ValueStr(p, a, 0, nullptr, "") == "true" ? 1 : 0;
}

static uintptr_t PlayerController() {
  return FindFirst([](uintptr_t o) { return !IsDefault(o) && IsA(o, "PlayerController") && RdP(o + 0) && OuterOf(o) && ClassName(OuterOf(o)) == "Level"; });
}
uintptr_t ResolveObj(const std::string& s) {
  if (!g_ok || s.empty()) return 0;
  uint64_t v;
  if ((s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0) && ParseU64(s, v)) return IsObj(v) ? v : 0;
  std::string ls = Lower(s);
  if (ls == "pc") return PlayerController();
  if (ls == "pawn") { uintptr_t pc = PlayerController(); if (!pc) return 0; uintptr_t p, a; std::string e; if (PropPath(pc, "AcknowledgedPawn", p, a, e)) return RdP(a); return 0; }
  bool path = s.find('/') != std::string::npos || s.find('.') != std::string::npos || s.find(':') != std::string::npos;
  uintptr_t best = FindFirst([&](uintptr_t o) { return path ? Lower(PathOf(o)) == ls : (Lower(ObjName(o)) == ls && !IsDefault(o)); });
  if (!best && !path) best = FindFirst([&](uintptr_t o) { return Lower(ObjName(o)) == ls; });
  return best;
}
static uintptr_t ResolveStruct(const std::string& s) {
  uintptr_t o = FindFirst([&](uintptr_t x) { std::string cn = ClassName(x); return (cn == "Class" || cn == "ScriptStruct" || cn == "BlueprintGeneratedClass" || cn == "WidgetBlueprintGeneratedClass") && Lower(ObjName(x)) == Lower(s); });
  if (o) return o;
  o = ResolveObj(s); if (!o) return 0;
  std::string cn = ClassName(o); return (cn.find("Class") != std::string::npos || cn == "ScriptStruct") ? o : ClassOf(o);
}

static std::string CallFunc(uintptr_t obj, const std::string& fname, const std::vector<std::string>& args, Out* o) {
  uintptr_t fn = FindFunc(ClassOf(obj), fname); if (!fn) return "no function " + fname + " on " + ClassName(obj);
  int size = 0; std::vector<uintptr_t> params;
  ForProps(fn, false, [&](uintptr_t q) { if (PropFlags(q) & 0x80) { params.push_back(q); size = std::max(size, PropOff(q) + PropElem(q) * std::max(1, PropDim(q))); } return true; });
  std::vector<uint8_t> buf(size + 16, 0); std::vector<std::wstring> keep; keep.reserve(16); std::string err; size_t ai = 0;
  for (uintptr_t q : params) {
    uint64_t fl = PropFlags(q); bool ret = fl & 0x400, out = (fl & 0x100) && !(fl & 0x8000000) && !(fl & 0x2);
    if (ret || out) continue;
    if (ai >= args.size()) return Fmt("missing arg %zu (%s %s)", ai, PropType(q).c_str(), PropName(q).c_str());
    if (!SetValue(q, (uintptr_t)buf.data() + PropOff(q), args[ai++], err, &keep)) return "arg " + PropName(q) + ": " + err;
  }
  void* pe = (void*)RdP(RdP(obj) + PE_VSLOT); int code = 0;
  if (!RunOnGame([&]() { code = SafeProcessEvent(pe, (void*)obj, (void*)fn, buf.data()); }, 5000, &err)) return err;
  if (code) return Fmt("ProcessEvent exception 0x%08x", code);
  std::string r = "ok";
  for (uintptr_t q : params) {
    uint64_t fl = PropFlags(q); if (!(fl & (0x400 | 0x100))) continue;
    uintptr_t qa = (uintptr_t)buf.data() + PropOff(q); std::string v = ValueStr(q, qa, 0, nullptr, "");
    r += Fmt("\n  %s %s = %s", (fl & 0x400) ? "return" : "out", PropName(q).c_str(), v.c_str());
    if (v.size() > 5 && v.compare(v.size() - 5, 5, "{...}") == 0)
      ForProps(PropX(q), true, [&](uintptr_t f) { r += Fmt("\n    %s = %s", PropName(f).c_str(), ValueStr(f, qa + PropOff(f), 0, nullptr, "").c_str()); return true; });
  }
  return r;
}

struct Watch { int id; uintptr_t obj, prop, addr; std::string label, last; };
static std::mutex g_wm; static std::vector<Watch> g_watches; static int g_watchId;
void QuestTick();
void TickWatches() {
  QuestTick();
  std::lock_guard<std::mutex> l(g_wm);
  for (auto& w : g_watches) { std::string v = ValueStr(w.prop, w.addr, 0, nullptr, ""); if (v != w.last) { LOG("watch %d %s: %s -> %s (tick %llu)", w.id, w.label.c_str(), w.last.c_str(), v.c_str(), H->tickCount); w.last = v; } }
}

static void DumpObj(Out& o, uintptr_t a) {
  o.f("0x%llx %s %s", a, ClassName(a).c_str(), PathOf(a).c_str());
  uintptr_t c = ClassOf(a); std::string supers; for (uintptr_t s = c; s; s = SuperOf(s)) supers += (supers.empty() ? "" : " > ") + ObjName(s);
  o.f("class chain: %s", supers.c_str()); o.f("outer: %s", DescribeObj(OuterOf(a)).c_str());
  for (uintptr_t s = c; s; s = SuperOf(s)) {
    int k = 0; ForProps(s, false, [&](uintptr_t) { k++; return true; }); if (!k) continue;
    o.f("-- %s", ObjName(s).c_str());
    ForProps(s, false, [&](uintptr_t q) {
      std::string v = ValueStr(q, a + PropOff(q), 0, nullptr, "");
      o.f("  +0x%04x %-32s %-18s %s", PropOff(q), PropName(q).c_str(), PropType(q).c_str(), v.c_str());
      if (v.size() > 5 && v.compare(v.size() - 5, 5, "{...}") == 0) StructStr(PropX(q), a + PropOff(q), 1, &o, "");
      return true;
    });
  }
}

static int g_funcOff = -1;
static int DetectFuncOff() {
  if (g_funcOff >= 0) return g_funcOff;
  uintptr_t actor = FindFirst([](uintptr_t o) { return ObjName(o) == "Actor" && ClassName(o) == "Class"; });
  uintptr_t f = actor ? FindFunc(actor, "K2_GetActorLocation") : 0; if (!f) return -1;
  uintptr_t f2 = FindFunc(actor, "K2_GetActorRotation"), f3 = FindFunc(actor, "K2_SetActorLocation"), ts; size_t tn;
  if (!f2 || !f3 || !SigText(H->base, &ts, &tn)) return -1;
  auto inText = [&](uintptr_t o, int off) { uintptr_t v = RdP(o + off); return v >= ts && v < ts + tn; };
  for (int off = 0xB0; off < 0x110; off += 8) if (inText(f, off) && inText(f2, off) && inText(f3, off)) { g_funcOff = off; break; }
  LOG("UFunction::Func offset = 0x%x", g_funcOff); return g_funcOff;
}
static std::string WriteRows(const std::string& file, const char* header, const std::vector<std::string>& rows) {
  FILE* f; if (fopen_s(&f, file.c_str(), "wb")) return "cannot open " + file;
  fputs(header, f); for (auto& r : rows) { fputs(r.c_str(), f); fputc('\n', f); } fclose(f); return "";
}
static std::string DumpFuncs(const std::string& file) {
  std::string err; std::vector<std::string> rows; int fo = -1;
  bool ok = RunOnGame([&]() {
    fo = DetectFuncOff(); if (fo < 0) return;
    int n = NumObjects();
    for (int i = 0; i < n; i++) {
      uintptr_t x = ObjAt(i); if (!x) continue; uintptr_t c = ClassOf(x); if (!c) continue;
      std::string cn = ObjName(c); if (cn != "Function" && cn != "DelegateFunction" && cn != "SparseDelegateFunction") continue;
      uintptr_t fn = RdP(x + fo); uint32_t fl = Rd<uint32_t>(x + 0xB0); uintptr_t ow = OuterOf(x);
      rows.push_back(Fmt("%s\t%s\t%s\t%08x\t%llx\t%s", ow ? ObjName(ow).c_str() : "?", ObjName(x).c_str(), cn.c_str(), fl,
                         InModule(fn) ? (unsigned long long)(fn - H->base) : 0ULL, ow ? PathOf(ow).c_str() : "?"));
    }
  }, 300000, &err);
  if (!ok) return err; if (fo < 0) return "cannot detect UFunction::Func offset";
  std::string e = WriteRows(file, "owner\tfunc\tkind\tflags\tfunc_rva\towner_path\n", rows); if (!e.empty()) return e;
  return Fmt("%zu functions -> %s (Func@0x%x)", rows.size(), file.c_str(), fo);
}
static std::string DumpSdk(const std::string& file) {
  std::string err; std::vector<std::string> rows; int structs = 0;
  static const std::set<std::string> kinds = { "Class", "ScriptStruct", "BlueprintGeneratedClass", "WidgetBlueprintGeneratedClass", "AnimBlueprintGeneratedClass", "UserDefinedStruct" };
  auto sub = [](uintptr_t q) { uintptr_t x = PropX(q); return IsObj(x) ? ObjName(x) : std::string(); };
  bool ok = RunOnGame([&]() {
    int n = NumObjects();
    for (int i = 0; i < n; i++) {
      uintptr_t s = ObjAt(i); if (!s) continue; uintptr_t c = ClassOf(s); if (!c) continue;
      std::string cn = ObjName(c); if (!kinds.count(cn)) continue;
      uintptr_t sp = SuperOf(s); std::string sn = ObjName(s), spn = sp && IsObj(sp) ? ObjName(sp) : "";
      rows.push_back(Fmt("S\t%s\t%s\t%s\t0x%x\t\t\t\t\t\t%s", sn.c_str(), cn.c_str(), spn.c_str(), Rd<int>(s + S_SIZE), PathOf(s).c_str()));
      structs++;
      ForProps(s, false, [&](uintptr_t q) {
        std::string ty = PropType(q), ex = sub(q);
        if (ty == "EnumProperty") { uintptr_t e = PropX(q, 1); if (IsObj(e)) ex = ObjName(e); }
        if (ty == "ArrayProperty" || ty == "SetProperty" || ty == "MapProperty") { uintptr_t in = PropInner(q); if (in) ex = PropType(in) + ":" + sub(in); }
        rows.push_back(Fmt("P\t%s\t\t\t\t%s\t%s\t0x%x\t%d\t%d\t%s", sn.c_str(), PropName(q).c_str(), ty.c_str(), PropOff(q), PropElem(q), PropDim(q), ex.c_str()));
        return true;
      });
    }
  }, 300000, &err);
  if (!ok) return err;
  std::string e = WriteRows(file, "rec\tstruct\tkind\tsuper\tsize\tprop\ttype\toffset\telem\tdim\textra\n", rows); if (!e.empty()) return e;
  return Fmt("%d structs, %zu rows -> %s", structs, rows.size(), file.c_str());
}
static std::string DumpVtables(const std::string& file) {
  std::string err; std::vector<std::string> rows;
  bool ok = RunOnGame([&]() {
    int n = NumObjects();
    for (int i = 0; i < n; i++) {
      uintptr_t x = ObjAt(i); if (!x) continue;
      if (!(Rd<uint32_t>(x + 0x8) & 0x10)) continue;
      uintptr_t c = ClassOf(x); if (!c) continue;
      uintptr_t vt = RdP(x); if (!InModule(vt)) continue;
      std::string slots; int k = 0;
      for (; k < 4096; k++) { uintptr_t v = RdP(vt + k * 8); if (!InText(v)) break; slots += Fmt(k ? ",%llx" : "%llx", (unsigned long long)(v - H->base)); }
      uintptr_t s = SuperOf(c);
      std::string cf;
      for (int off = 0xB0; off < 0x180; off += 8) { uintptr_t v = RdP(c + off); if (InText(v)) cf += Fmt("%s%x:%llx", cf.empty() ? "" : ",", off, (unsigned long long)(v - H->base)); }
      rows.push_back(Fmt("%s\t%s\t%s\t%llx\t%d\t%s\t", ObjName(c).c_str(), s ? ObjName(s).c_str() : "-", ClassName(c).c_str(),
                         (unsigned long long)(vt - H->base), k, PathOf(c).c_str()) + slots + "\t" + cf);
    }
  }, 300000, &err);
  if (!ok) return err;
  std::string e = WriteRows(file, "class\tsuper\tmeta\tvt_rva\tnslots\tclass_path\tslots\tuclass_fns\n", rows); if (!e.empty()) return e;
  return Fmt("%zu CDO vtables -> %s", rows.size(), file.c_str());
}

static std::string DumpFieldVt(const std::string& file) {
  std::string err; std::vector<std::string> rows;
  bool ok = RunOnGame([&]() {
    std::map<uintptr_t, uintptr_t> fc;
    int n = NumObjects();
    for (int i = 0; i < n; i++) {
      uintptr_t x = ObjAt(i); if (!x) continue; std::string cn = ClassName(x);
      if (cn.find("Class") == std::string::npos && cn.find("Struct") == std::string::npos && cn.find("Function") == std::string::npos) continue;
      ForProps(x, false, [&](uintptr_t p) { uintptr_t k = RdP(p + FF_CLASS); if (k && !fc.count(k)) fc[k] = p; return true; });
    }
    std::map<uintptr_t, uintptr_t> all = fc;
    for (auto& e : fc) for (uintptr_t s = RdP(e.first + 0x20), g = 0; s && g < 16; s = RdP(s + 0x20), g++) if (!all.count(s)) all[s] = RdP(s + 0x28);
    for (auto& e : all) {
      uintptr_t k = e.first, f = e.second ? e.second : RdP(k + 0x28); if (!f) continue;
      uintptr_t vt = RdP(f); if (!InModule(vt)) continue;
      std::string slots; int j = 0;
      for (; j < 1024; j++) { uintptr_t v = RdP(vt + j * 8); if (!InText(v)) break; slots += Fmt(j ? ",%llx" : "%llx", (unsigned long long)(v - H->base)); }
      uintptr_t s = RdP(k + 0x20);
      rows.push_back(Fmt("F%s\t%s\tFFieldClass\t%llx\t%d\t-\t%s", NameStr(Rd<uint64_t>(k)).c_str(), s ? ("F" + NameStr(Rd<uint64_t>(s))).c_str() : "-",
                         (unsigned long long)(vt - H->base), j, slots.c_str()));
    }
  }, 300000, &err);
  if (!ok) return err;
  std::string e = WriteRows(file, "class\tsuper\tmeta\tvt_rva\tnslots\tclass_path\tslots\n", rows); if (!e.empty()) return e;
  return Fmt("%zu FField class vtables -> %s", rows.size(), file.c_str());
}

bool UPropAddr(const std::string& obj, const std::string& path, uintptr_t& addr, int& size, std::string& type, std::string& val, std::string& err) {
  if (!EnsureUObj(err)) return false;
  uintptr_t o = ResolveObjCached(obj); if (!o) { err = "cannot resolve object " + obj; return false; }
  uintptr_t prop, a; if (!PropPath(o, path, prop, a, err)) return false;
  type = PropType(prop); val = ValueStr(prop, a, 0, nullptr, "");
  bool indexed = !path.empty() && path.back() == ']';
  size = PropElem(prop) * (indexed ? 1 : std::max(1, PropDim(prop)));
  if (type == "BoolProperty") { a += Rd<uint8_t>(prop + P_SIZE + 1); size = 1; }
  addr = a; return true;
}

size_t ObjSize(uintptr_t a) {
  if (!IsObj(a)) return 0;
  uintptr_t c = ClassOf(a); int s = c ? Rd<int>(c + S_SIZE) : 0;
  return s > 0 ? (size_t)s : 0;
}
static std::string PropAtIn(uintptr_t st, int off, int depth) {
  std::string r;
  ForProps(st, true, [&](uintptr_t q) {
    int po = PropOff(q), sz = PropElem(q) * std::max(1, PropDim(q));
    if (off < po || off >= po + sz) return true;
    r = PropName(q);
    if (depth > 0 && PropType(q) == "StructProperty" && PropX(q)) { std::string s = PropAtIn(PropX(q), off - po, depth - 1); if (!s.empty()) r += "." + s; }
    return false;
  });
  return r;
}
std::string PropAtOffset(uintptr_t obj, int off) {
  if (!IsObj(obj)) return "";
  return PropAtIn(ClassOf(obj), off, 2);
}

static void SnapFields(uintptr_t st, uintptr_t base, uintptr_t addr, int len, const std::string& prefix, int depth, std::vector<std::string>& out) {
  ForProps(st, true, [&](uintptr_t q) {
    if (out.size() > 4000) return false;
    int off = PropOff(q), sz = PropElem(q) * std::max(1, PropDim(q));
    if (off < 0 || off + sz > len + 0x10000) return true;
    std::string ty = PropType(q), nm = prefix + PropName(q);
    std::string v = ValueStr(q, addr + off, 0, nullptr, "");
    std::string hex; if (off + sz <= len && sz <= 64) { std::vector<uint8_t> b(sz); if (SafeRead(b.data(), addr + off, sz)) for (uint8_t x : b) hex += Fmt("%02x", x); }
    out.push_back(jo::Obj().str("name", nm).str("type", ty).num("offset", base + off).num("size", sz).str("value", v).str("hex", hex).done());
    if (ty == "StructProperty" && depth > 0 && PropX(q)) SnapFields(PropX(q), base + off, addr + off, len, nm + ".", depth - 1, out);
    return true;
  });
}

static std::string SafeName(const std::string& s) { std::string r; for (char c : s) r += (isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.') ? c : '_'; return r.empty() ? "snap" : r; }

static void CmdSnap(const std::vector<std::string>& t, Out& o) {
  if (t.size() < 4) { o.line("usage: snap <addr|obj+off|obj> <len|auto> <name> [type=<Class|Struct>]"); return; }
  std::string err; uint64_t a;
  if (!EvalAddr(t[1], a, err)) { o.line("error: " + err); return; }
  uintptr_t st = 0; std::string tname;
  for (size_t i = 4; i < t.size(); i++) if (t[i].rfind("type=", 0) == 0) { st = ResolveStruct(t[i].substr(5)); if (!st) { o.line("cannot resolve type " + t[i].substr(5)); return; } }
  if (!st && IsObj(a)) st = ClassOf(a);
  if (st) tname = ObjName(st);
  uint64_t len = 0; bool autoLen = t[2] == "auto" || t[2] == "0";
  if (!autoLen && !ParseU64(t[2], len)) { o.line("error: bad length " + t[2]); return; }
  if (autoLen) { if (!st) { o.line("error: len auto needs an object or type="); return; } len = (uint64_t)Rd<int>(st + S_SIZE); }
  if (len == 0 || len > (1u << 20)) { o.f("error: bad length %llu (1..1048576)", len); return; }
  std::vector<uint8_t> buf((size_t)len);
  if (!SafeRead(buf.data(), a, buf.size())) { o.f("error: cannot read %llu bytes at 0x%llx", len, a); return; }
  std::string hex; hex.reserve(buf.size() * 2); for (uint8_t b : buf) { char x[3]; sprintf_s(x, "%02x", b); hex += x; }
  std::vector<std::string> fields; if (st) SnapFields(st, 0, a, (int)len, "", 1, fields);
  SYSTEMTIME s; GetLocalTime(&s);
  std::string name = SafeName(t[3]), dir = LogDir() + "\\snaps";
  CreateDirectoryA(dir.c_str(), nullptr);
  std::string base = dir + "\\" + name;
  jo::Obj j; j.str("name", name).hex("addr", a).str("type", tname).num("len", (long long)len).str("md5", Md5Bytes(buf.data(), buf.size()));
  if (InModule(a)) j.hex("rva", a - H->base);
  j.str("taken", Fmt("%04d-%02d-%02dT%02d:%02d:%02d", s.wYear, s.wMonth, s.wDay, s.wHour, s.wMinute, s.wSecond)).str("hex", hex).raw("fields", jo::Arr(fields));
  FILE* f; std::string js = j.done();
  if (fopen_s(&f, (base + ".json").c_str(), "wb")) { o.line("error: cannot write " + base + ".json"); return; }
  fwrite(js.data(), 1, js.size(), f); fclose(f);
  if (!fopen_s(&f, (base + ".bin").c_str(), "wb")) { fwrite(buf.data(), 1, buf.size(), f); fclose(f); }
  JsonSetData(jo::Obj().str("name", name).hex("addr", a).str("type", tname).num("len", (long long)len).num("fields", (long long)fields.size()).str("json", base + ".json").str("bin", base + ".bin").done());
  o.f("snap %s 0x%llx len %llu type %s fields %zu -> %s.json (+ .bin)", name.c_str(), a, len, tname.empty() ? "-" : tname.c_str(), fields.size(), base.c_str());
}

static void CmdScene(const std::vector<std::string>& t, Out& o) {
  uintptr_t pc = PlayerController(), pawn = 0, level = 0, world = 0; std::string err;
  if (pc) { uintptr_t p, a; if (PropPath(pc, "AcknowledgedPawn", p, a, err)) pawn = RdP(a); level = OuterOf(pc); world = level ? OuterOf(level) : 0; }
  if (world && ClassName(world) != "World") world = 0;
  std::string wname = world ? ObjName(world) : "", wpath = world ? PathOf(world) : "", pcls = pawn ? ClassName(pawn) : "", loc;
  if (pawn) { uintptr_t p, a; if (PropPath(pawn, "RootComponent.RelativeLocation", p, a, err)) loc = ValueStr(p, a, 0, nullptr, ""); }
  std::vector<std::string> req, props;
  for (size_t i = 1; i < t.size(); i++) {
    if (t[i].rfind("require=", 0) == 0) req.push_back(t[i].substr(8));
    else if (t[i].rfind("prop=", 0) == 0) props.push_back(t[i].substr(5));
  }
  std::vector<std::string> need;
  for (auto& r : req) { size_t s = 0, e; do { e = r.find(',', s); if (e == std::string::npos) e = r.size(); if (e > s) need.push_back(r.substr(s, e - s)); s = e + 1; } while (s <= r.size()); }
  std::vector<std::string> fails;
  for (auto& n : need) {
    std::string ln = Lower(n);
    if (n == "world") { if (!world) fails.push_back("no world"); }
    else if (n == "pc") { if (!pc) fails.push_back("no player controller"); }
    else if (n == "pawn") { if (!pawn) fails.push_back("no player pawn"); }
    else if (ln.rfind("level:", 0) == 0) { if (Lower(wpath).find(ln.substr(6)) == std::string::npos) fails.push_back("level '" + n.substr(6) + "' not in '" + wpath + "'"); }
    else if (ln.rfind("class:", 0) == 0) { if (Lower(pcls).find(ln.substr(6)) == std::string::npos) fails.push_back("pawn class '" + n.substr(6) + "' not in '" + pcls + "'"); }
    else fails.push_back("unknown requirement '" + n + "' (world pc pawn level:<text> class:<text>)");
  }
  std::vector<std::string> pj;
  o.f("scene world=%s pc=0x%llx pawn=0x%llx pawn_class=%s ticks=%llu", wname.empty() ? "-" : wname.c_str(), pc, pawn, pcls.empty() ? "-" : pcls.c_str(), H->tickCount);
  o.f("  world_path=%s", wpath.empty() ? "-" : wpath.c_str());
  o.f("  pawn_location=%s", loc.empty() ? "-" : loc.c_str());
  for (auto& p : props) {
    uintptr_t pr, a; std::string v = "n/a";
    if (pawn && PropPath(pawn, p, pr, a, err)) v = ValueStr(pr, a, 0, nullptr, ""); else if (pawn) v = "error: " + err;
    o.f("  prop %s = %s", p.c_str(), v.c_str()); pj.push_back(jo::Obj().str("path", p).str("value", v).done());
  }
  bool loaded = world && pc && pawn;
  jo::Obj d; d.b("world_loaded", loaded).str("world", wname).str("world_path", wpath).hex("pc", pc).hex("pawn", pawn).str("pawn_class", pcls).str("pawn_location", loc).num("ticks", (long long)H->tickCount).raw("props", jo::Arr(pj));
  if (!need.empty()) { d.b("require_ok", fails.empty()).raw("failures", jo::StrArr(fails)); }
  JsonSetData(d.done());
  if (!fails.empty()) { std::string m; for (auto& f : fails) m += (m.empty() ? "" : "; ") + f; o.line("scene FAIL: " + m); JsonSetOk(false); }
  else if (!need.empty()) o.line("scene OK: all requirements met");
}

typedef void (*UidGetFn)(uintptr_t, int*);
static int SafeUidGet(uintptr_t fn, uintptr_t itf) { int v = -1; __try { ((UidGetFn)fn)(itf, &v); } __except (EXCEPTION_EXECUTE_HANDLER) { v = -1; } return v; }
static int UidOf(uintptr_t a) {
  if (!a || !IsA(a, "Obj")) return -1;
  uintptr_t itf = a + 0x648, vt = RdP(itf), fn = vt ? RdP(vt + 0x10) : 0;
  return fn ? SafeUidGet(fn, itf) : -1;
}

struct NpcRow { uintptr_t addr; int uid, idx, hidden; float hp; double loc[3]; bool haveLoc; double dist; std::string cls; };

static void CmdNpcs(const std::vector<std::string>& t, Out& o) {
  uopt::Args ao = uopt::Parse(t, 1);
  uintptr_t pawn = ResolveObj("pawn"); double nearLoc[3] = {}; bool haveNear = false, rFilter = ao.Has("r");
  std::string ne = ao.Has("near") ? ao.Str("near") : "pawn";
  if (uopt::ParseXyz(ne, nearLoc)) haveNear = true;
  else { uintptr_t no = ResolveObj(ne); if (no) haveNear = ActorLoc(no, nearLoc); else if (ao.Has("near")) { o.line("error: cannot resolve near=" + ne); return; } }
  double rad = ao.Dbl("r", 0); long long wantUid = ao.Has("uid") ? ao.Int("uid", 0) : -1; int newN = (int)ao.Int("new", 0);
  int mx = (int)ao.Int("max", ao.pos.empty() ? 50 : atoi(ao.pos[0].c_str()));
  std::string cf = Lower(ao.Str("class")); bool all = ao.Flag("all");
  std::vector<NpcRow> rows; int n = NumObjects();
  for (int i = 0; i < n; i++) {
    uintptr_t x = ObjAt(i); if (!x || IsDefault(x) || !IsA(x, "Obj")) continue;
    if (x == pawn && !all) continue;
    NpcRow r{}; r.addr = x; r.cls = ClassName(x);
    if (!cf.empty() && Lower(r.cls).find(cf) == std::string::npos) continue;
    r.uid = UidOf(x);
    if (wantUid >= 0 && r.uid != wantUid) continue;
    r.idx = Rd<int>(x + O_INDEX); r.haveLoc = ActorLoc(x, r.loc); r.dist = 1e18;
    if (r.haveLoc && haveNear) r.dist = uopt::Dist(r.loc, nearLoc);
    if (rFilter && (!r.haveLoc || r.dist > rad)) continue;
    uintptr_t hc = RdP(x + 0x650); r.hp = hc ? Rd<float>(hc + 0x148) : -1.0f;
    r.hidden = HiddenOf(x); rows.push_back(r);
  }
  std::stable_sort(rows.begin(), rows.end(), [](const NpcRow& a, const NpcRow& b) { return a.dist < b.dist; });
  if (newN > 0) { std::stable_sort(rows.begin(), rows.end(), [](const NpcRow& a, const NpcRow& b) { return a.idx > b.idx; }); if ((int)rows.size() > newN) rows.resize(newN); }
  int total = (int)rows.size(); if ((int)rows.size() > mx) rows.resize(mx);
  std::vector<std::string> js;
  for (auto& r : rows) {
    std::string d = r.dist < 1e17 ? Fmt("%.0f", r.dist) : "-", lc = r.haveLoc ? Fmt("%.1f,%.1f,%.1f", r.loc[0], r.loc[1], r.loc[2]) : "-";
    o.f("uid=%d 0x%llx %s hp=%g dist=%s loc=%s hidden=%d idx=%d", r.uid, (unsigned long long)r.addr, r.cls.c_str(), r.hp, d.c_str(), lc.c_str(), r.hidden < 0 ? 0 : r.hidden, r.idx);
    jo::Obj j; j.num("uid", r.uid).hex("addr", r.addr).str("class", r.cls).dbl("hp", r.hp).b("hidden", r.hidden > 0).num("idx", r.idx);
    if (r.dist < 1e17) j.dbl("dist", r.dist);
    if (r.haveLoc) j.raw("loc", jo::Arr({ jo::Dbl(r.loc[0]), jo::Dbl(r.loc[1]), jo::Dbl(r.loc[2]) }));
    js.push_back(j.done());
  }
  JsonSetData(jo::Obj().raw("npcs", jo::Arr(js)).num("total", total).done());
  o.f("%d npcs", total);
}

struct CompRow { uintptr_t addr, parent; std::string cls, name, socket, mesh, anim; bool scene, vis, hidden, haveLoc; double loc[3]; int depth; };

static std::string PropObjName(uintptr_t o, const char* path) {
  uintptr_t p, a; std::string err;
  if (!PropPath(o, path, p, a, err)) return "";
  uintptr_t v = RdP(a); return v && IsObj(v) ? ObjName(v) : "";
}
static bool PropBool(uintptr_t o, const char* path, bool def) {
  uintptr_t p, a; std::string err;
  if (!PropPath(o, path, p, a, err)) return def;
  return ValueStr(p, a, 0, nullptr, "") == "true";
}

static void CmdComps(const std::vector<std::string>& t, Out& o) {
  if (t.size() < 2) { o.line("usage: comps <actor> [filter=<substr>] [flat]"); return; }
  uintptr_t actor = ResolveObj(t[1]); if (!actor) { o.line("cannot resolve object " + t[1]); return; }
  uopt::Args ao = uopt::Parse(t, 2); std::string flt = Lower(ao.Str("filter")); bool flat = ao.Flag("flat");
  std::vector<CompRow> rows; int n = NumObjects();
  for (int i = 0; i < n; i++) {
    uintptr_t x = ObjAt(i); if (!x || OuterOf(x) != actor || IsDefault(x) || !IsA(x, "ActorComponent")) continue;
    CompRow r{}; r.addr = x; r.cls = ClassName(x); r.name = ObjName(x); r.scene = IsA(x, "SceneComponent");
    if (r.scene) {
      uintptr_t p, a; std::string err;
      if (PropPath(x, "AttachParent", p, a, err)) r.parent = RdP(a);
      if (PropPath(x, "AttachSocketName", p, a, err)) { r.socket = ValueStr(p, a, 0, nullptr, ""); if (r.socket == "None") r.socket.clear(); }
      if (PropPath(x, "RelativeLocation", p, a, err)) r.haveLoc = ReadVec3(p, a, r.loc);
      r.vis = PropBool(x, "bVisible", true); r.hidden = PropBool(x, "bHiddenInGame", false);
      if (IsA(x, "SkeletalMeshComponent")) {
        r.mesh = PropObjName(x, "SkeletalMeshAsset"); if (r.mesh.empty()) r.mesh = PropObjName(x, "SkeletalMesh");
        uintptr_t ai = 0; if (PropPath(x, "AnimScriptInstance", p, a, err)) ai = RdP(a);
        if (ai && IsObj(ai)) r.anim = ClassName(ai);
      }
    }
    rows.push_back(r);
  }
  std::map<uintptr_t, size_t> byAddr; for (size_t i = 0; i < rows.size(); i++) byAddr[rows[i].addr] = i;
  std::vector<size_t> order; std::vector<bool> seen(rows.size(), false);
  std::function<void(size_t, int)> walk = [&](size_t i, int d) {
    if (seen[i]) return; seen[i] = true; rows[i].depth = flat ? 0 : d; order.push_back(i);
    for (size_t k = 0; k < rows.size(); k++) if (!seen[k] && rows[k].scene && rows[k].parent == rows[i].addr) walk(k, d + 1);
  };
  uintptr_t rootc = 0; { uintptr_t p, a; std::string err; if (PropPath(actor, "RootComponent", p, a, err)) rootc = RdP(a); }
  if (flat) for (size_t i = 0; i < rows.size(); i++) walk(i, 0);
  else {
    if (byAddr.count(rootc)) walk(byAddr[rootc], 0);
    for (size_t i = 0; i < rows.size(); i++) if (rows[i].scene && !byAddr.count(rows[i].parent)) walk(i, 0);
    for (size_t i = 0; i < rows.size(); i++) walk(i, 0);
  }
  std::vector<std::string> js; int shown = 0;
  for (size_t i : order) {
    const CompRow& r = rows[i];
    if (!flt.empty() && Lower(r.cls).find(flt) == std::string::npos && Lower(r.name).find(flt) == std::string::npos) continue;
    std::string pn = "-";
    if (r.parent && IsObj(r.parent)) { pn = ObjName(r.parent); if (!byAddr.count(r.parent) && OuterOf(r.parent)) pn = ObjName(OuterOf(r.parent)) + "." + pn; }
    std::string line = std::string(r.depth * 2, ' ') + Fmt("0x%llx %s %s", (unsigned long long)r.addr, r.cls.c_str(), r.name.c_str());
    jo::Obj j; j.hex("addr", r.addr).str("class", r.cls).str("name", r.name).num("depth", r.depth);
    if (r.scene) {
      line += Fmt(" socket=%s parent=%s", r.socket.empty() ? "-" : r.socket.c_str(), pn.c_str());
      line += r.haveLoc ? Fmt(" loc=%g,%g,%g", r.loc[0], r.loc[1], r.loc[2]) : " loc=-";
      line += Fmt(" vis=%d hidden=%d", r.vis ? 1 : 0, r.hidden ? 1 : 0);
      if (!r.mesh.empty()) line += " mesh=" + r.mesh;
      if (!r.anim.empty()) line += " anim=" + r.anim;
      j.str("socket", r.socket).str("parent", pn).b("visible", r.vis).b("hidden", r.hidden).str("mesh", r.mesh).str("anim", r.anim);
      if (r.haveLoc) j.raw("loc", jo::Arr({ jo::Dbl(r.loc[0]), jo::Dbl(r.loc[1]), jo::Dbl(r.loc[2]) }));
    }
    o.line(line); js.push_back(j.done()); shown++;
  }
  JsonSetData(jo::Obj().hex("actor", actor).raw("components", jo::Arr(js)).num("total", shown).done());
  o.f("%d components", shown);
}

struct PCall { uintptr_t obj = 0, fn = 0, ret = 0; std::vector<uint8_t> buf; int code = 0; };

static bool PrepCall(uintptr_t obj, const std::string& fname, const std::vector<std::string>& args, PCall& pc, std::string& err) {
  uintptr_t fn = FindFunc(ClassOf(obj), fname); if (!fn) { err = "no function " + fname + " on " + ClassName(obj); return false; }
  int size = 0; std::vector<uintptr_t> params;
  ForProps(fn, false, [&](uintptr_t q) { if (PropFlags(q) & 0x80) { params.push_back(q); size = std::max(size, PropOff(q) + PropElem(q) * std::max(1, PropDim(q))); } return true; });
  pc.obj = obj; pc.fn = fn; pc.ret = 0; pc.code = 0; pc.buf.assign((size_t)size + 16, 0);
  std::vector<std::wstring> keep; keep.reserve(8); size_t ai = 0;
  for (uintptr_t q : params) {
    uint64_t fl = PropFlags(q); bool ret = (fl & 0x400) != 0, out = (fl & 0x100) && !(fl & 0x8000000) && !(fl & 0x2);
    if (ret) pc.ret = q;
    if (ret || out) continue;
    if (ai >= args.size()) { err = Fmt("%s: missing arg %zu", fname.c_str(), ai); return false; }
    if (!SetValue(q, (uintptr_t)pc.buf.data() + PropOff(q), args[ai++], err, &keep)) { err = fname + " arg " + PropName(q) + ": " + err; return false; }
  }
  return true;
}
static bool RunCalls(std::vector<PCall>& cs, std::string& err) {
  if (cs.empty()) return true;
  return RunOnGame([&]() { for (auto& c : cs) { void* pe = (void*)RdP(RdP(c.obj) + PE_VSLOT); c.code = SafeProcessEvent(pe, (void*)c.obj, (void*)c.fn, c.buf.data()); } }, 8000, &err);
}
static bool CallXf(const PCall& c, pse::Xf& x) {
  if (c.code || !c.ret) return false;
  const uint8_t* b = c.buf.data() + PropOff(c.ret);
  if (PropElem(c.ret) >= 80) { double d[7]; memcpy(d, b, sizeof d); for (int i = 0; i < 4; i++) x.q[i] = d[i]; for (int i = 0; i < 3; i++) x.t[i] = d[4 + i]; }
  else { float d[7]; memcpy(d, b, sizeof d); for (int i = 0; i < 4; i++) x.q[i] = d[i]; for (int i = 0; i < 3; i++) x.t[i] = d[4 + i]; }
  return true;
}
static bool CallInt(const PCall& c, int& v) { if (c.code || !c.ret) return false; memcpy(&v, c.buf.data() + PropOff(c.ret), 4); return true; }

static uintptr_t FindPoseMesh(uintptr_t actor) {
  uintptr_t best = 0; int n = NumObjects();
  for (int i = 0; i < n; i++) {
    uintptr_t x = ObjAt(i); if (!x || OuterOf(x) != actor || IsDefault(x) || !IsA(x, "SkeletalMeshComponent")) continue;
    if (ObjName(x).rfind("CharacterMesh", 0) == 0) return x;
    if (!best) best = x;
  }
  return best;
}

static bool ResolveBones(uintptr_t mesh, const std::string& spec, std::vector<std::string>& bones, std::string& err) {
  std::string sp = Lower(spec);
  if (sp != "legs" && sp != "all") { bones = pse::BoneSet(sp == "hands" || sp == "arms" ? sp : spec); return true; }
  std::vector<PCall> one(1);
  if (!PrepCall(mesh, "GetNumBones", {}, one[0], err) || !RunCalls(one, err)) return false;
  int nb = 0; if (!CallInt(one[0], nb) || nb <= 0 || nb > 4096) { err = "GetNumBones returned no bones"; return false; }
  std::vector<PCall> names((size_t)nb);
  for (int i = 0; i < nb; i++) if (!PrepCall(mesh, "GetBoneName", { std::to_string(i) }, names[i], err)) return false;
  if (!RunCalls(names, err)) return false;
  for (int i = 0; i < nb; i++) {
    if (names[i].code || !names[i].ret) continue;
    uint64_t fn; memcpy(&fn, names[i].buf.data() + PropOff(names[i].ret), 8);
    std::string n = NameStr(fn);
    if (sp == "all" ? (int)bones.size() < 200 : pse::IsLegBone(n)) bones.push_back(n);
  }
  return true;
}

static bool BoneRows(uintptr_t mesh, const std::vector<std::string>& bones, int dispSpace, std::vector<pse::Row>& rows, std::string& err) {
  static const int spaces[3] = { 0, 2, 3 };
  std::vector<PCall> calls(bones.size() * 4);
  for (size_t b = 0; b < bones.size(); b++) {
    if (!PrepCall(mesh, "GetBoneIndex", { bones[b] }, calls[b * 4], err)) return false;
    for (int s = 0; s < 3; s++) if (!PrepCall(mesh, "GetSocketTransform", { bones[b], std::to_string(s == 0 ? dispSpace : spaces[s]) }, calls[b * 4 + 1 + s], err)) return false;
  }
  if (!RunCalls(calls, err)) return false;
  for (size_t b = 0; b < bones.size(); b++) {
    int idx = -1; if (!CallInt(calls[b * 4], idx) || idx < 0) continue;
    pse::Row r; r.bone = bones[b];
    if (!CallXf(calls[b * 4 + 1], r.disp) || !CallXf(calls[b * 4 + 2], r.comp) || !CallXf(calls[b * 4 + 3], r.par)) continue;
    rows.push_back(r);
  }
  return true;
}

static std::string Vec3S(const double v[3]) { return Fmt("%.1f,%.1f,%.1f", v[0], v[1], v[2]); }

static void CmdPose(const std::vector<std::string>& t, Out& o) {
  if (t.size() < 2) { o.line("usage: pose <actor> [bones=hands|arms|legs|all|a,b,c] [ref=<actor>] [comps] [space=world|comp|parent]"); return; }
  uopt::Args ao = uopt::Parse(t, 2);
  uintptr_t actor = ResolveObj(t[1]); if (!actor) { o.line("cannot resolve object " + t[1]); return; }
  std::string err, spaceName = Lower(ao.Str("space", "world")), boneSpec = ao.Str("bones", "hands");
  int dispSpace = spaceName == "world" ? 0 : spaceName == "comp" ? 2 : spaceName == "parent" ? 3 : -1;
  if (dispSpace < 0) { o.line("error: space must be world, comp or parent"); return; }
  uintptr_t mesh = FindPoseMesh(actor); if (!mesh) { o.line("error: no SkeletalMeshComponent on " + t[1]); return; }
  uintptr_t refActor = 0, refMesh = 0;
  if (ao.Has("ref")) {
    refActor = ResolveObj(ao.Str("ref")); if (!refActor) { o.line("cannot resolve object " + ao.Str("ref")); return; }
    refMesh = FindPoseMesh(refActor); if (!refMesh) { o.line("error: no SkeletalMeshComponent on " + ao.Str("ref")); return; }
  }
  std::vector<std::string> bones;
  if (!ResolveBones(mesh, boneSpec, bones, err)) { o.line("error: " + err); return; }
  std::vector<pse::Row> rows, refRows;
  if (!BoneRows(mesh, bones, dispSpace, rows, err)) { o.line("error: " + err); return; }
  if (refMesh) {
    if (!BoneRows(refMesh, bones, dispSpace, refRows, err)) { o.line("error: ref: " + err); return; }
    for (auto& r : rows) for (auto& q : refRows) if (q.bone == r.bone) { r.haveRef = true; r.refComp = q.comp; r.refPar = q.par; r.refDisp = q.disp; }
  }
  pse::Finish(rows);
  o.f("pose %s mesh %s space=%s bones=%zu%s", ObjName(actor).c_str(), ObjName(mesh).c_str(), spaceName.c_str(), rows.size(), refMesh ? (" ref=" + ObjName(refActor)).c_str() : "");
  o.line("bone             loc(cm)                 rot(roll,pitch,yaw)     d_loc  d_rot  flag");
  std::vector<std::string> jr; int flags = 0;
  for (auto& r : rows) {
    double e[3]; pse::Euler(r.disp.q, e);
    std::string ref = r.haveRef ? Fmt("%6.1f %6.1f", r.dLoc, r.dRot) : "     -      -";
    o.f("%-16s %-23s %-23s %s  %s", r.bone.c_str(), Vec3S(r.disp.t).c_str(), Vec3S(e).c_str(), ref.c_str(), r.flag.c_str());
    if (!r.flag.empty()) flags++;
    jo::Obj j; j.str("bone", r.bone).raw("loc", jo::Arr({ jo::Dbl(r.disp.t[0]), jo::Dbl(r.disp.t[1]), jo::Dbl(r.disp.t[2]) })).raw("rot", jo::Arr({ jo::Dbl(e[0]), jo::Dbl(e[1]), jo::Dbl(e[2]) })).dbl("roll", r.roll);
    if (r.haveRef) j.raw("ref_loc", jo::Arr({ jo::Dbl(r.refDisp.t[0]), jo::Dbl(r.refDisp.t[1]), jo::Dbl(r.refDisp.t[2]) })).dbl("d_loc", r.dLoc).dbl("d_rot", r.dRot);
    j.str("flag", r.flag); jr.push_back(j.done());
  }
  std::vector<std::string> jc; int ncomps = 0;
  if (ao.Flag("comps")) {
    struct CR { uintptr_t c; std::string socket; };
    std::vector<CR> cr; int n = NumObjects();
    for (int i = 0; i < n; i++) {
      uintptr_t x = ObjAt(i); if (!x || OuterOf(x) != actor || IsDefault(x) || x == mesh || !IsA(x, "SceneComponent")) continue;
      uintptr_t p, a; if (!PropPath(x, "AttachParent", p, a, err) || RdP(a) != mesh) continue;
      if (!PropPath(x, "AttachSocketName", p, a, err)) continue;
      std::string sk = ValueStr(p, a, 0, nullptr, ""); if (sk.empty() || sk == "None") continue;
      cr.push_back({ x, sk });
    }
    std::vector<PCall> cc(cr.size() * 2);
    bool okc = true;
    for (size_t i = 0; i < cr.size() && okc; i++) okc = PrepCall(cr[i].c, "K2_GetComponentToWorld", {}, cc[i * 2], err) && PrepCall(mesh, "GetSocketTransform", { cr[i].socket, "0" }, cc[i * 2 + 1], err);
    if (!okc || !RunCalls(cc, err)) { o.line("error: comps: " + err); }
    else {
      o.line("comp             class                   socket          gap(cm)  ang(deg)  flag");
      for (size_t i = 0; i < cr.size(); i++) {
        pse::Xf cx, sx; if (!CallXf(cc[i * 2], cx) || !CallXf(cc[i * 2 + 1], sx)) continue;
        double gap = pse::Dist3(cx.t, sx.t), ang = pse::QuatAngle(cx.q, sx.q); std::string fl = pse::CompFlag(gap, IsA(cr[i].c, "MeshComponent"));
        o.f("%-16s %-23s %-15s %7.1f %9.1f  %s", ObjName(cr[i].c).c_str(), ClassName(cr[i].c).c_str(), cr[i].socket.c_str(), gap, ang, fl.c_str());
        if (!fl.empty()) flags++;
        ncomps++;
        jc.push_back(jo::Obj().str("name", ObjName(cr[i].c)).str("class", ClassName(cr[i].c)).str("socket", cr[i].socket).dbl("gap", gap).dbl("ang", ang).str("flag", fl).done());
      }
    }
  }
  JsonSetData(jo::Obj().hex("actor", actor).hex("mesh", mesh).hex("ref", refActor).str("space", spaceName).raw("rows", jo::Arr(jr)).raw("comps", jo::Arr(jc)).num("flags", flags).done());
  o.f("%zu bones %d comps, %d flags", rows.size(), ncomps, flags);
}
#include "ue_quest.inc"
#include "ue_containers.inc"
bool HandleUObj(const std::string& c, const std::vector<std::string>& t, const std::string& line, Out& o) {
  static const char* cmds[] = { "find", "obj", "get", "set", "call", "funcs", "classes", "props", "watchprop", "unwatch", "con", "uinfo", "dumpfuncs", "dumpvtables", "dumpsdk", "dumpfieldvt", "scene", "snap", "npcs", "uid", "comps", "pose", "quest", "tarray", "tset", "tmap", "layoutcheck" };
  if (std::find_if(std::begin(cmds), std::end(cmds), [&](const char* x) { return c == x; }) == std::end(cmds)) return false;
  { std::string why; if (!EnsureUObj(why)) { o.line(why); return true; } }
  std::string err;
  if (c == "scene") { CmdScene(t, o); return true; }
  if (c == "snap") { CmdSnap(t, o); return true; }
  if (c == "npcs") { CmdNpcs(t, o); return true; }
  if (c == "comps") { CmdComps(t, o); return true; }
  if (c == "pose") { CmdPose(t, o); return true; }
  if (c == "quest") { CmdQuest(t, o); return true; }
  if (c == "tarray" || c == "tset" || c == "tmap") { CmdContainer(c, t, o); return true; }
  if (c == "layoutcheck") { CmdLayoutCheck(t, o); return true; }
  if (c == "uinfo") { o.f("objects=%d stride=%d arr=0x%llx FField name@0x%x offset@0x%x sizeof(FProperty)=0x%x", NumObjects(), ITEM_STRIDE, g_arr, FF_NAME, P_OFFSET, P_SIZE); return true; }
  if (c == "dumpfieldvt") { if (t.size() < 2) { o.line("usage: dumpfieldvt <file>"); return true; } o.line(DumpFieldVt(t[1])); return true; }
  if (c == "dumpfuncs" || c == "dumpvtables" || c == "dumpsdk") {
    if (t.size() < 2) { o.line("usage: " + c + " <file>"); return true; }
    o.line(c == "dumpfuncs" ? DumpFuncs(t[1]) : c == "dumpsdk" ? DumpSdk(t[1]) : DumpVtables(t[1])); return true;
  }
  if (c == "find") {
    if (t.size() < 2) { o.line("usage: find <substr|class:Name|isa:Name> [max] [near=<obj|x,y,z>] [r=<cm>] [loc] [new=<N>] [since=<idx>] [hidden=0|1] [nodef]"); return true; }
    std::string q = t[1]; int mode = 0;
    uopt::Args ao = uopt::Parse(t, 2);
    int mx = ao.pos.empty() ? 50 : atoi(ao.pos[0].c_str());
    if (q.rfind("class:", 0) == 0) { mode = 1; q = q.substr(6); } else if (q.rfind("isa:", 0) == 0) { mode = 2; q = q.substr(4); }
    bool wantNear = ao.Has("near"), wantLoc = ao.Flag("loc"), nodef = ao.Flag("nodef"), ext = wantNear || wantLoc || ao.Has("new") || ao.Has("since") || ao.Has("hidden");
    int newN = (int)ao.Int("new", 0), hid = ao.Has("hidden") ? (int)ao.Int("hidden", 0) : -1;
    long long since = ao.Has("since") ? ao.Int("since", 0) : -1;
    double nearLoc[3] = {}, rad = ao.Dbl("r", 500);
    if (wantNear) {
      std::string ne = ao.Str("near");
      if (!uopt::ParseXyz(ne, nearLoc)) {
        uintptr_t no = ResolveObj(ne);
        if (!no) { o.line("error: cannot resolve near=" + ne); return true; }
        if (!ActorLoc(no, nearLoc)) { o.line("error: near=" + ne + " has no RootComponent.RelativeLocation"); return true; }
      }
    }
    std::string lq = Lower(q); int hits = 0, n = NumObjects(); std::vector<uopt::Row> rows;
    for (int i = 0; i < n; i++) {
      uintptr_t x = ObjAt(i); if (!x) continue;
      bool m = mode == 0 ? Lower(ObjName(x)).find(lq) != std::string::npos : mode == 1 ? Lower(ClassName(x)) == lq : (!IsDefault(x) && IsA(x, q));
      if (!m) continue;
      if (!ext) { if (!(nodef && IsDefault(x)) && hits++ < mx) o.f("0x%llx  %-28s %s", x, ClassName(x).c_str(), PathOf(x).c_str()); continue; }
      if (nodef && IsDefault(x)) continue;
      uopt::Row r; r.addr = x; r.idx = Rd<int>(x + O_INDEX);
      if (since >= 0 && r.idx <= since) continue;
      if (hid >= 0) { r.hidden = HiddenOf(x); if (r.hidden != hid) continue; }
      if (wantNear || wantLoc) { r.haveLoc = ActorLoc(x, r.loc); }
      if (wantNear) { if (!r.haveLoc) continue; r.dist = uopt::Dist(r.loc, nearLoc); if (r.dist > rad) continue; }
      rows.push_back(r);
    }
    if (!ext) { o.f("%d hits", hits); return true; }
    uopt::SelectRows(rows, wantNear, newN);
    int total = (int)rows.size(); if ((int)rows.size() > mx) rows.resize(mx);
    std::vector<std::string> js;
    for (auto& r : rows) {
      if (hid < 0 && (wantLoc || wantNear)) r.hidden = HiddenOf(r.addr);
      std::string cn = ClassName(r.addr), pa = PathOf(r.addr), line = Fmt("0x%llx  %-28s %s  idx=%d", (unsigned long long)r.addr, cn.c_str(), pa.c_str(), r.idx);
      jo::Obj j; j.hex("addr", r.addr).str("class", cn).str("path", pa).num("idx", r.idx);
      if (wantLoc || wantNear) {
        line += r.haveLoc ? Fmt("  loc=%.1f,%.1f,%.1f", r.loc[0], r.loc[1], r.loc[2]) : "  loc=-";
        if (r.haveLoc) j.raw("loc", jo::Arr({ jo::Dbl(r.loc[0]), jo::Dbl(r.loc[1]), jo::Dbl(r.loc[2]) }));
        if (wantNear) { line += Fmt("  dist=%.0f", r.dist); j.dbl("dist", r.dist); }
        line += Fmt("  hidden=%d", r.hidden < 0 ? 0 : r.hidden); j.b("hidden", r.hidden > 0);
      }
      o.line(line); js.push_back(j.done());
    }
    JsonSetData(jo::Obj().raw("objects", jo::Arr(js)).num("total", total).done());
    o.f("%d hits", total); return true;
  }
  if (c == "classes") {
    std::string lq = t.size() > 1 ? Lower(t[1]) : ""; int hits = 0, n = NumObjects();
    for (int i = 0; i < n; i++) { uintptr_t x = ObjAt(i); if (!x) continue; std::string cn = ClassName(x); if (cn != "Class" && cn != "BlueprintGeneratedClass") continue; std::string nm = ObjName(x); if (Lower(nm).find(lq) == std::string::npos) continue; if (hits++ < 200) o.f("0x%llx %s : %s", x, PathOf(x).c_str(), SuperOf(x) ? ObjName(SuperOf(x)).c_str() : "-"); }
    o.f("%d classes", hits); return true;
  }
  if (c == "con") {
    std::string cmd = line.substr(line.find(' ') == std::string::npos ? line.size() : line.find(' ') + 1);
    if (cmd.size() >= 2 && cmd.front() == '"' && cmd.back() == '"') cmd = cmd.substr(1, cmd.size() - 2);
    { size_t sp = cmd.find(' '); std::string nm = cmd.substr(0, sp), val = sp == std::string::npos ? "" : cmd.substr(sp + 1);
      while (!val.empty() && val[0] == ' ') val.erase(0, 1);
      std::string r = CVarCmd(nm, sp == std::string::npos || val.empty() ? nullptr : &val); if (!r.empty()) { o.line("cvar " + r); return true; } }
    uintptr_t ksl = ResolveObj("Default__KismetSystemLibrary"), pc = PlayerController();
    if (!ksl || !pc) { o.line("need KismetSystemLibrary CDO and a PlayerController"); return true; }
    o.line(CallFunc(ksl, "ExecuteConsoleCommand", { Fmt("0x%llx", pc), cmd, "null" }, &o)); return true;
  }
  if (c == "unwatch") { std::lock_guard<std::mutex> l(g_wm); if (t.size() > 1) { int id = atoi(t[1].c_str()); g_watches.erase(std::remove_if(g_watches.begin(), g_watches.end(), [&](Watch& w) { return w.id == id; }), g_watches.end()); } else g_watches.clear(); o.f("%zu watches left", g_watches.size()); return true; }
  if (t.size() < 2) { o.line("usage: " + c + " <obj> ..."); return true; }
  if (c == "funcs" || c == "props") {
    uintptr_t st = ResolveStruct(t[1]); if (!st) { o.line("cannot resolve " + t[1]); return true; }
    std::string lf = t.size() > 2 ? Lower(t[2]) : "";
    for (uintptr_t s = st; s; s = SuperOf(s)) {
      if (c == "props") { ForProps(s, false, [&](uintptr_t q) { if (Lower(PropName(q)).find(lf) != std::string::npos) o.f("%-20s +0x%04x %3d %-32s %s", ObjName(s).c_str(), PropOff(q), PropElem(q), PropName(q).c_str(), PropType(q).c_str()); return true; }); continue; }
      for (uintptr_t f = RdP(s + S_CHILDREN); f; f = RdP(f + F_NEXT)) {
        if (ClassName(f) != "Function" && ClassName(f) != "DelegateFunction") continue;
        std::string nm = ObjName(f); if (Lower(nm).find(lf) == std::string::npos) continue;
        std::string sig; ForProps(f, false, [&](uintptr_t q) { uint64_t fl = PropFlags(q); if (fl & 0x80) sig += Fmt("%s%s %s%s", sig.empty() ? "" : ", ", PropType(q).c_str(), PropName(q).c_str(), (fl & 0x400) ? "(ret)" : (fl & 0x100) ? "(out)" : ""); return true; });
        uintptr_t native = 0; for (int off = 0xB0; off < 0x100; off += 8) { uintptr_t v = RdP(f + off); if (InText(v)) { native = v; break; } }
        o.f("%-20s %-40s (%s) %s", ObjName(s).c_str(), nm.c_str(), sig.c_str(), native ? Sym(native).c_str() : "");
      }
    }
    return true;
  }
  uintptr_t obj = ResolveObj(t[1]); if (!obj) { o.line("cannot resolve object " + t[1]); return true; }
  if (c == "obj") { DumpObj(o, obj); return true; }
  if (c == "uid") {
    if (!IsA(obj, "Obj")) { o.line("error: not an Obj actor: " + ClassName(obj)); return true; }
    int u = UidOf(obj);
    JsonSetData(jo::Obj().num("uid", u).hex("addr", obj).str("class", ClassName(obj)).done());
    o.f("uid=%d 0x%llx %s", u, (unsigned long long)obj, ClassName(obj).c_str()); return true;
  }
  if (c == "call") {
    if (t.size() < 3) { o.line("usage: call <obj> <Func> [args]"); return true; }
    o.line(CallFunc(obj, t[2], std::vector<std::string>(t.begin() + 3, t.end()), &o)); return true;
  }
  if (t.size() < 3) { o.line("usage: " + c + " <obj> <prop.path> [value]"); return true; }
  uintptr_t prop, addr; if (!PropPath(obj, t[2], prop, addr, err)) { o.line("error: " + err); return true; }
  if (c == "get") { std::string v = ValueStr(prop, addr, 0, nullptr, ""); o.f("%s %s @0x%llx = %s", PropType(prop).c_str(), t[2].c_str(), addr, v.c_str()); if (v.size() > 5 && v.compare(v.size() - 5, 5, "{...}") == 0) StructStr(PropX(prop), addr, 2, &o, ""); return true; }
  if (c == "set") {
    if (t.size() < 4) { o.line("usage: set <obj> <prop.path> <value>"); return true; }
    std::string before = ValueStr(prop, addr, 0, nullptr, "");
    bool ok = SetValue(prop, addr, t[3], err, nullptr);
    o.f("%s %s: %s -> %s", ok ? "set" : ("FAILED " + err).c_str(), t[2].c_str(), before.c_str(), ValueStr(prop, addr, 0, nullptr, "").c_str()); return true;
  }
  if (c == "watchprop") {
    std::lock_guard<std::mutex> l(g_wm); Watch w{ ++g_watchId, obj, prop, addr, ObjName(obj) + "." + t[2], ValueStr(prop, addr, 0, nullptr, "") };
    g_watches.push_back(w); o.f("watch %d %s = %s (changes go to the log)", w.id, w.label.c_str(), w.last.c_str()); return true;
  }
  return false;
}

static std::string g_why;
static rty::Gate g_gate;
static std::mutex g_gm;
static void SetWhy(const std::string& w) { std::lock_guard<std::mutex> l(g_gm); g_why = w; }

void InitUObj() {
  bool loud; { std::lock_guard<std::mutex> l(g_gm); loud = g_gate.Loud(GetTickCount64(), 30000); }
  if (!g_fnToString) g_fnToString = SigFind(H->base, SIG_FNAME_TOSTRING);
  if (!g_fnCtor) g_fnCtor = SigFind(H->base, SIG_FNAME_CTOR);
  if (!g_arr) g_arr = SigFind(H->base, SIG_GUOBJECTARRAY, SIG_GUOBJECTARRAY_RIP);
  static uintptr_t conMgr; if (!conMgr) conMgr = SigFind(H->base, SIG_GCONSOLEMANAGER, SIG_GCONSOLEMANAGER_RIP);
  g_ok = false;
  if (loud) LOG("sig rva: GUObjectArray 0x%llx FName::ToString 0x%llx FName ctor 0x%llx GConsoleManager 0x%llx", g_arr ? g_arr - H->base : 0, g_fnToString ? g_fnToString - H->base : 0, g_fnCtor ? g_fnCtor - H->base : 0, conMgr ? conMgr - H->base : 0);
  if (!g_arr || !g_fnToString || !g_fnCtor) {
    SetWhy(std::string("signature scan failed for") + (g_arr ? "" : " GUObjectArray") + (g_fnToString ? "" : " FName::ToString") + (g_fnCtor ? "" : " FName ctor"));
    if (loud) LOG("signature scan failed, UObject access off"); return;
  }
  int num = Rd<int>(g_arr + 0x24), maxe = Rd<int>(g_arr + 0x20);
  if (loud) LOG("GUObjectArray 0x%llx num=%d max=%d chunks=0x%llx", g_arr, num, maxe, RdP(g_arr + 0x10));
  if (num <= 0 || num > maxe) { SetWhy("GUObjectArray empty (no world yet)"); if (loud) LOG("GUObjectArray not ready/invalid"); return; }
  for (int s : { 24, 16, 32 }) { ITEM_STRIDE = s; uintptr_t o1 = ObjAt(1), o2 = ObjAt(2); if (o1 && o2 && Rd<int>(o1 + O_INDEX) == 1 && Rd<int>(o2 + O_INDEX) == 2) break; }
  g_ok = true;
  uintptr_t o0 = ObjAt(0);
  if (loud) LOG("stride=%d obj0=0x%llx name=%s class=%s", ITEM_STRIDE, o0, ObjName(o0).c_str(), ClassName(o0).c_str());
  uintptr_t actor = FindFirst([](uintptr_t o) { return ObjName(o) == "Actor" && ClassName(o) == "Class"; });
  uintptr_t pawn = FindFirst([](uintptr_t o) { return ObjName(o) == "Pawn" && ClassName(o) == "Class"; });
  if (!actor || !pawn) { SetWhy("Actor/Pawn class not found yet"); if (loud) LOG("Actor/Pawn class not found"); g_ok = false; return; }
  if (loud) LOG("Actor class 0x%llx super=%s", actor, ObjName(SuperOf(actor)).c_str());
  bool found = false;
  for (int nameOff : { 0x28, 0x20, 0x30 }) {
    FF_NAME = nameOff; FF_NEXT = nameOff - 8;
    uintptr_t pt = FindProp(actor, "PrimaryActorTick"), be = FindProp(pawn, "BaseEyeHeight");
    if (!pt || !be) continue;
    for (int off = nameOff + 0x10; off < nameOff + 0x40; off += 4) if (Rd<int>(pt + off) == 0x28 && Rd<int>(be + off) == 0x2BC) { P_OFFSET = off; found = true; break; }
    if (!found) continue;
    for (int off = nameOff + 0x8; off < P_OFFSET; off += 4) if (Rd<int>(be + off) == 1 && Rd<int>(be + off + 4) == 4) { P_ARRAYDIM = off; P_ELEMSIZE = off + 4; break; }
    P_FLAGS = (P_ELEMSIZE + 4 + 7) & ~7;
    for (int off = P_OFFSET + 4; off < P_OFFSET + 0x60; off += 8) { uintptr_t s = RdP(pt + off); if (IsObj(s) && ObjName(s) == "ActorTickFunction") { P_SIZE = off; break; } }
    break;
  }
  LOG("FField layout: found=%d name@0x%x next@0x%x dim@0x%x elem@0x%x flags@0x%x offset@0x%x sizeof=0x%x", found, FF_NAME, FF_NEXT, P_ARRAYDIM, P_ELEMSIZE, P_FLAGS, P_OFFSET, P_SIZE);
  g_ok = found;
  if (!found) SetWhy("FField layout not found");
  else { std::lock_guard<std::mutex> l(g_gm); g_why.clear(); g_gate.Reset(); }
}

static bool EnsureUObj(std::string& why) {
  if (g_ok) return true;
  uint64_t now = GetTickCount64();
  { std::lock_guard<std::mutex> l(g_gm);
    if (!g_gate.Try(now, 2000)) { why = Fmt("error: reflection not ready (%s); retry in %llu s", g_why.empty() ? "initialising" : g_why.c_str(), (unsigned long long)g_gate.WaitSec(now)); return false; } }
  InitUObj();
  if (g_ok) return true;
  std::string w; { std::lock_guard<std::mutex> l(g_gm); w = g_why; }
  why = "error: " + (w.empty() ? std::string("reflection not initialised") : w); return false;
}

