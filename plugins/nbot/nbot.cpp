#include <windows.h>
#include "../../src/home.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>
#include <intrin.h>
#pragma comment(lib, "user32.lib")
#include "s2dev_api.h"
#include "..\..\src\sig.h"
#include "nbot_scene.h"
#include "nbot_cine_math.h"

static const S2Api* api;
static bool g_on;
static int g_port;
static std::string g_io;
static uint64_t g_tick;

struct V3 { double x, y, z; };
static double D2(V3 a, V3 b) { return sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y)); }

static std::string Fmt(const char* f, ...) { char b[2048]; va_list ap; va_start(ap, f); vsnprintf(b, sizeof b, f, ap); va_end(ap); return b; }

static void Log(const char* f, ...)
{
  char b[2048]; va_list ap; va_start(ap, f); vsnprintf(b, sizeof b, f, ap); va_end(ap);
  SYSTEMTIME t; GetLocalTime(&t);
  FILE* fp = fopen((g_io + "\\nbot.log").c_str(), "ab");
  if (fp) { fprintf(fp, "%02d:%02d:%02d.%03d %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, b); fclose(fp); }
}

#include <map>
static std::map<std::string, uintptr_t> g_objc;
static uintptr_t g_pc, g_pawn; static uint64_t g_pcT;
static bool Valid(uintptr_t o) { char h[32]; snprintf(h, sizeof h, "0x%llx", (unsigned long long)o); return o && api->find(h) == o; }
static uintptr_t Find(const char* obj)
{
  std::string n = obj;
  if (n == "pc" || n == "pawn") {
    uint64_t now = GetTickCount64();
    if (!g_pc || now - g_pcT > 1000) {
      g_pcT = now;
      for (int k = 0; k < 2; k++) {
        if (k == 1 || !Valid(g_pc)) g_pc = api->find("pc");
        g_pawn = 0;
        if (g_pc) { char out[256] = {0}; api->get(g_pc, "AcknowledgedPawn", out, sizeof out); const char* x = strstr(out, "0x"); g_pawn = x ? (uintptr_t)_strtoui64(x, nullptr, 16) : 0; }
        if (g_pawn) break;
      }
    }
    return n == "pc" ? g_pc : g_pawn;
  }
  if (n.rfind("0x", 0) == 0) return api->find(obj);
  auto it = g_objc.find(n);
  if (it != g_objc.end() && Valid(it->second)) return it->second;
  uintptr_t o = api->find(obj); if (o) g_objc[n] = o; return o;
}
static std::string Call(const char* obj, const char* fn, const char* args)
{
  uintptr_t o = Find(obj); if (!o) return "noobj";
  char out[4096] = {0}; api->call(o, fn, args ? args : "", out, sizeof out); return out;
}
static std::string CallO(uintptr_t o, const char* fn, const char* args)
{
  if (!o) return "noobj";
  char out[4096] = {0}; api->call(o, fn, args ? args : "", out, sizeof out); return out;
}
static std::string Cmd(const std::string& line) { static char out[65536]; out[0] = 0; api->cmd(line.c_str(), out, sizeof out); return out; }

static bool ParseV(const std::string& s, V3& v)
{
  size_t a = s.find("X="), b = s.find("Y="), c = s.find("Z=");
  if (a == std::string::npos || b == std::string::npos || c == std::string::npos) return false;
  v.x = atof(s.c_str() + a + 2); v.y = atof(s.c_str() + b + 2); v.z = atof(s.c_str() + c + 2); return true;
}
static double ParseNum(const std::string& s, const char* key)
{
  size_t a = s.find(key); if (a == std::string::npos) return NAN;
  a = s.find('=', a); if (a == std::string::npos) return NAN; return atof(s.c_str() + a + 1);
}

static bool Pos(V3& v) { return ParseV(Call("pawn", "K2_GetActorLocation", ""), v); }
static bool Dead() { double h = ParseNum(Call("pawn", "GetHP", ""), "ReturnValue"); return !isnan(h) && h <= 0; }
static bool Rot(V3& r)
{
  std::string s = Call("pc", "GetControlRotation", "");
  double p = ParseNum(s, "Pitch="), y = ParseNum(s, "Yaw="); if (isnan(p) || isnan(y)) return false;
  r.x = p; r.y = y; r.z = 0; return true;
}
static void SetMove(double f, double r) { Call("pawn", "SetMoveVector", Fmt("%.3f,%.3f,0", f, r).c_str()); }
static void SetRot(double pitch, double yaw) { Call("pc", "SetControlRotation", Fmt("%.2f,%.2f,0", pitch, yaw).c_str()); }
static void LookAt(V3 from, V3 to)
{
  double dx = to.x - from.x, dy = to.y - from.y, dz = to.z - (from.z + 60);
  SetRot(atan2(dz, sqrt(dx * dx + dy * dy)) * 57.29578, atan2(dy, dx) * 57.29578);
}
static bool RawTeleport(V3 v) { std::string r = Call("pawn", "K2_SetActorLocation", Fmt("%.1f,%.1f,%.1f false true", v.x, v.y, v.z).c_str()); return r.find("ReturnValue = true") != std::string::npos; }

static uintptr_t* PfSlot()
{
  static const DWORD* idx = nullptr; static bool init = false;
  if (!init) {
    init = true;
    auto* b = (const uint8_t*)GetModuleHandleW(nullptr);
    auto* nt = (const IMAGE_NT_HEADERS64*)(b + ((const IMAGE_DOS_HEADER*)b)->e_lfanew);
    const auto& dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (dd.VirtualAddress) idx = (const DWORD*)((const IMAGE_TLS_DIRECTORY64*)(b + dd.VirtualAddress))->AddressOfIndex;
  }
  if (!idx) return nullptr;
  auto* arr = (uint8_t**)__readgsqword(0x58); if (!arr || !arr[*idx]) return nullptr;
  return (uintptr_t*)(arr[*idx] + 0xd88);
}
typedef void(__fastcall* PEFn)(void*, void*, void*);
static int PeSeh(uintptr_t obj, uintptr_t fn, void* parms)
{
  uintptr_t* sl = PfSlot(); uintptr_t v0 = sl ? *sl : 0;
  __try { PEFn pe = *(PEFn*)(*(uintptr_t*)obj + 0x278); pe((void*)obj, (void*)fn, parms); return 0; }
  __except (EXCEPTION_EXECUTE_HANDLER) { if (sl && *sl != v0) *sl = v0; return (int)GetExceptionCode(); }
}
typedef void(__fastcall* N1)(uintptr_t);
static int NatSeh(uintptr_t f, uintptr_t a)
{
  uintptr_t* sl = PfSlot(); uintptr_t v0 = sl ? *sl : 0;
  __try { ((N1)f)(a); return 0; }
  __except (EXCEPTION_EXECUTE_HANDLER) { if (sl && *sl != v0) *sl = v0; return (int)GetExceptionCode(); }
}
typedef void(__fastcall* N2)(uintptr_t, uintptr_t);
static int NatSeh2(uintptr_t f, uintptr_t a, uintptr_t b)
{
  if (!f) return -1;
  uintptr_t* sl = PfSlot(); uintptr_t v0 = sl ? *sl : 0;
  __try { ((N2)f)(a, b); return 0; }
  __except (EXCEPTION_EXECUTE_HANDLER) { if (sl && *sl != v0) *sl = v0; return (int)GetExceptionCode(); }
}
#define NB_SIG_EQUIP "56 57 53 48 83 EC 30 89 D3 48 89 CE 48 8B 05 ?? ?? ?? ?? 48 31 E0 48 89 44 24 ?? 48 8B B9 78 06 00 00 8A 89 4B 02 00 00"
#define NB_SIG_HOLD "41 57 41 56 41 55 41 54 56 57 53 48 81 EC 90 00 00 00 48 8B 05 ?? ?? ?? ?? 48 31 E0 48 89 84 24 88 00 00 00 83 3D ?? ?? ?? ?? FF 0F 84 ?? ?? ?? ?? 89 D3"
#define NB_SIG_SM "48 31 E0 48 89 84 24 80 00 00 00 48 8B 1D ?? ?? ?? ?? BE FF FF FF FF 48 85 DB 74 78 48 89 CF 48 8B 05"
static const uint8_t kTapTime[] = { 0x48, 0x8B, 0x05 };
static const uint8_t kTapUse[] = { 0xF2, 0x0F, 0x5A, 0x80, 0xA0, 0x00, 0x00, 0x00, 0xF3, 0x0F, 0x5C, 0x41, 0x40 };
struct NatAddrs { bool done = false; uintptr_t equip = 0, hold = 0, tap = 0, time = 0, sm = 0; };
static NatAddrs g_nat;
static bool BytesAt(uintptr_t a, const uint8_t* b, size_t n) { __try { return memcmp((const void*)a, b, n) == 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
static uintptr_t RdSeh(uintptr_t a) { __try { return *(uintptr_t*)a; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static bool TapResolve()
{
  uintptr_t vt = api->addr("SingleClickIPU::vftable"), t = vt ? RdSeh(vt + 93 * 8) : 0;
  if (!t || !BytesAt(t + 0x1a, kTapTime, sizeof kTapTime) || !BytesAt(t + 0x21, kTapUse, sizeof kTapUse)) return false;
  g_nat.time = t + 0x21 + *(const int32_t*)(t + 0x1d); g_nat.tap = t;
  api->log("nbot tap by SingleClickIPU::vftable[93]: tap=exe+0x%llx time=exe+0x%llx", (unsigned long long)(t - api->base), (unsigned long long)(g_nat.time - api->base));
  return true;
}
static const NatAddrs& Nat()
{
  if (g_nat.done) { if (!g_nat.tap) TapResolve(); return g_nat; }
  g_nat.done = true;
  uintptr_t b = api->base;
  g_nat.equip = SigFind(b, NB_SIG_EQUIP);
  g_nat.hold = SigFind(b, NB_SIG_HOLD);
  g_nat.sm = SigFind(b, NB_SIG_SM, 14);
  TapResolve();
  api->log("nbot natives by signature: equip=exe+0x%llx hold=exe+0x%llx tap=exe+0x%llx time=exe+0x%llx scenario=exe+0x%llx (0 = not found)",
    (unsigned long long)(g_nat.equip ? g_nat.equip - b : 0), (unsigned long long)(g_nat.hold ? g_nat.hold - b : 0), (unsigned long long)(g_nat.tap ? g_nat.tap - b : 0),
    (unsigned long long)(g_nat.time ? g_nat.time - b : 0), (unsigned long long)(g_nat.sm ? g_nat.sm - b : 0));
  return g_nat;
}
static int HoldNat(int released) { return NatSeh2(Nat().hold, 0, (uintptr_t)released); }
static uintptr_t ObjAddr(const char* obj, const char* path)
{
  uintptr_t o = Find(obj); if (!o) return 0;
  char out[512] = {0}; api->get(o, path, out, sizeof out);
  const char* x = strstr(out, "0x"); return x ? (uintptr_t)_strtoui64(x, nullptr, 16) : 0;
}
struct Floor { bool hit, walk; float dist; V3 imp; };
static bool FloorAt(V3 loc, float range, Floor& f)
{
  static uintptr_t fn = 0; if (!fn) fn = Find("/Script/Engine.CharacterMovementComponent:K2_ComputeFloorDist");
  uintptr_t cm = ObjAddr("pawn", "CharacterMovement"); if (!fn || !cm) return false;
  alignas(16) uint8_t b[0x180] = {0};
  *(double*)(b + 0) = loc.x; *(double*)(b + 8) = loc.y; *(double*)(b + 16) = loc.z;
  *(float*)(b + 0x18) = range; *(float*)(b + 0x1c) = range; *(float*)(b + 0x20) = 30.f;
  if (PeSeh(cm, fn, b)) return false;
  uint8_t bits = b[0x28]; f.hit = (bits & 1) != 0; f.walk = (bits & 2) != 0; f.dist = *(float*)(b + 0x2c);
  f.imp = V3{ *(double*)(b + 0x38 + 0x28), *(double*)(b + 0x38 + 0x30), *(double*)(b + 0x38 + 0x38) };
  return true;
}
static int MoveMode()
{
  uintptr_t o = Find("pawn"); char out[256] = {0}; if (!o) return -1;
  api->get(o, "CharacterMovement.MovementMode", out, sizeof out); return out[0] >= '0' && out[0] <= '9' ? atoi(out) : -1;
}
static bool OnGround(Floor* fo = nullptr)
{
  V3 p; if (!Pos(p)) return false; if (MoveMode() != 1) return false;
  Floor f{}; if (!FloorAt(p, 200.f, f)) return false; if (fo) *fo = f; return f.hit && f.walk;
}
static std::vector<V3> g_crumbs;
static uint64_t g_crumbT;
static void Crumb()
{
  uint64_t now = GetTickCount64(); if (now - g_crumbT < 1000) return; g_crumbT = now;
  V3 p; if (!Pos(p) || !OnGround()) return;
  if (!g_crumbs.empty() && D2(g_crumbs.back(), p) < 150 && fabs(g_crumbs.back().z - p.z) < 100) return;
  g_crumbs.push_back(p); if (g_crumbs.size() > 40) g_crumbs.erase(g_crumbs.begin());
}
static std::string g_seg = "-";
struct Settle { bool on = false; uint64_t t0 = 0, okSince = 0; V3 dst{}; std::string why; };
static Settle g_set;
static int g_tpOk = 0;
static double g_hpg = 0; static uint64_t g_hpgT = 0; static int g_hpgN = 0; static double g_hpMin = 1e9; static int g_hpS = 0;
static void TickHpGuard()
{
  uint64_t now = GetTickCount64(); if (g_hpg <= 0 || now < g_hpgT) return; g_hpgT = now + 600;
  double h = ParseNum(Call("pawn", "GetHP", ""), "ReturnValue");
  if (!isnan(h)) { if (h < g_hpMin) { g_hpMin = h; if (h < 50) Log("hpmin %.1f (below 50)", h); } g_hpS++; }
  if (isnan(h) || h <= 0 || h >= g_hpg - 0.5) return;
  Call("pawn", "ForceSetHP", Fmt("%.1f", g_hpg).c_str()); g_hpgN++;
  if (g_hpgN <= 5 || g_hpgN % 50 == 0) Log("hpguard #%d hp %.1f -> %.1f", g_hpgN, h, g_hpg);
}
static bool SafeTeleport(V3 v, const std::string& why)
{
  Floor f{}; bool good = false;
  const double offs[] = { 100, 300, 600, -100 };
  for (double o : offs) { V3 probe{ v.x, v.y, v.z + o }; if (FloorAt(probe, 900.f, f) && f.hit && f.walk && f.dist >= 0 && f.imp.z > v.z - 700 && f.imp.z < v.z + 400) { good = true; break; } }
  if (!good) { Log("BENCH WARN tp refused seg=%s why=%s to=%.0f,%.0f,%.0f no walkable floor (hit=%d walk=%d dist=%.0f)", g_seg.c_str(), why.c_str(), v.x, v.y, v.z, f.hit ? 1 : 0, f.walk ? 1 : 0, f.dist); return false; }
  V3 dst{ v.x, v.y, f.imp.z + 100 };
  V3 p{}; Pos(p);
  Call("pawn", "SetMoveVector", "0,0,0");
  bool ok = RawTeleport(dst);
  Log("BENCH WARN teleport seg=%s why=%s from=%.0f,%.0f,%.0f to=%.0f,%.0f,%.0f floorz=%.0f ok=%d", g_seg.c_str(), why.c_str(), p.x, p.y, p.z, dst.x, dst.y, dst.z, f.imp.z, ok ? 1 : 0);
  if (ok) { g_tpOk++; Log("shortcut: teleport #%d why=%s seg=%s %.0f m; upgrade when the walk reaches this point by input", g_tpOk, why.c_str(), g_seg.c_str(), D2(p, dst) / 100); }
  if (ok) { g_set = Settle(); g_set.on = true; g_set.t0 = GetTickCount64(); g_set.dst = dst; g_set.why = why; }
  return ok;
}
static bool ToCrumb(const std::string& why)
{
  while (!g_crumbs.empty()) { V3 c = g_crumbs.back(); g_crumbs.pop_back(); if (SafeTeleport(c, why)) return true; }
  Log("BENCH WARN no breadcrumb left seg=%s why=%s", g_seg.c_str(), why.c_str());
  return false;
}
static bool TickSettle()
{
  if (!g_set.on) return false;
  uint64_t now = GetTickCount64();
  if (OnGround()) { if (!g_set.okSince) g_set.okSince = now; if (now - g_set.okSince >= 500) { g_set.on = false; Log("settled after %s in %llu ms", g_set.why.c_str(), (unsigned long long)(now - g_set.t0)); } return true; }
  g_set.okSince = 0;
  V3 p{}; Pos(p);
  if (now - g_set.t0 > 4000 || p.z < g_set.dst.z - 400) { g_set.on = false; Log("BENCH WARN settle failed after %s at %.0f,%.0f,%.0f", g_set.why.c_str(), p.x, p.y, p.z); ToCrumb("settle-fail"); }
  return true;
}
static uint64_t g_fallT;
static bool FallGuard()
{
  int mm = MoveMode(); V3 p{}; Pos(p);
  uint64_t now = GetTickCount64();
  bool below = !g_crumbs.empty() && p.z < g_crumbs.back().z - 600;
  if (mm == 3 || below) {
    if (!g_fallT) g_fallT = now;
    if (now - g_fallT > 2500 || below) { g_fallT = 0; Call("pawn", "SetMoveVector", "0,0,0"); Log("BENCH WARN fall seg=%s at %.0f,%.0f,%.0f mode=%d", g_seg.c_str(), p.x, p.y, p.z, mm); ToCrumb("fall"); return true; }
  } else g_fallT = 0;
  return false;
}
static double GameNow()
{
  __try { if (!Nat().time) return -1; uintptr_t gp = *(uintptr_t*)Nat().time; if (!gp) return -1; return *(double*)(gp + 0xa0); }
  __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
static std::string Tap()
{
  std::string a = Call("pawn", "GetInteractedActor", "");
  size_t q = a.find("ReturnValue = ");
  if (q == std::string::npos || a.find("ReturnValue = null") != std::string::npos || a.find("ReturnValue = None") != std::string::npos) return "err nothing to interact: " + a;
  double now = GameNow(); if (now < 0) return "err no game time";
  alignas(16) static uint8_t ipu[0x100]; memset(ipu, 0, sizeof ipu); *(float*)(ipu + 0x40) = (float)now;
  if (!Nat().tap) return "err tap native not found by signature"; int r = NatSeh(Nat().tap, (uintptr_t)ipu);
  std::string an = a.substr(q + 14); while (!an.empty() && (an.back() == '\n' || an.back() == '\r')) an.pop_back();
  Log("tap seg=%s actor=%s rc=0x%x", g_seg.c_str(), an.c_str(), r);
  return Fmt("tap rc=0x%x actor=%s", r, an.c_str());
}

enum GoalKind { G_NONE, G_GOTO, G_FIRE, G_HOLDMOVE, G_HOLD, G_USE };
struct Goal {
  GoalKind kind = G_NONE; int id = 0; std::string state = "idle", detail, seg = "-";
  std::vector<V3> pts; size_t idx = 0; double tol = 150; uint64_t t0 = 0, until = 0;
  V3 lastPos{}; uint64_t lastCheck = 0; int stuck = 0, tps = 0, jumps = 0; double timeout = 90; bool allowTp = true; double best = 1e18; bool nav = false; V3 dest{}; int replans = 0;
};
static Goal g;
static bool ReadPts(uintptr_t po, std::vector<V3>& out)
{
  __try {
    uintptr_t d = *(uintptr_t*)(po + 0x38); int n = *(int*)(po + 0x40);
    if (!d || n <= 0 || n > 4096) return false;
    for (int i = 0; i < n; i++) { double* v = (double*)(d + i * 24); out.push_back(V3{ v[0], v[1], v[2] }); }
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static std::string g_navWhy;
static std::string OneLine(std::string s, size_t n) { for (auto& c : s) if (c == '\n' || c == '\r') c = ' '; return s.substr(0, n); }
static bool NavPath(V3 from, V3 to, std::vector<V3>& out, bool& partial)
{
  std::string r = Call("/Script/NavigationSystem.Default__NavigationSystemV1", "FindPathToLocationSynchronously", Fmt("pawn %.1f,%.1f,%.1f %.1f,%.1f,%.1f null null", from.x, from.y, from.z, to.x, to.y, to.z).c_str());
  size_t q = r.find("ReturnValue = 0x"); if (q == std::string::npos) { g_navWhy = "no path object: " + OneLine(r, 160); return false; }
  uintptr_t po = (uintptr_t)_strtoui64(r.c_str() + q + 14, nullptr, 16);
  std::string ip = CallO(po, "IsPartial", ""); partial = ip.find("= true") != std::string::npos;
  std::vector<V3> raw; if (!ReadPts(po, raw)) { g_navWhy = Fmt("path 0x%llx has no points partial=%d", (unsigned long long)po, partial ? 1 : 0); return false; }
  g_navWhy = Fmt("ok raw=%zu partial=%d", raw.size(), partial ? 1 : 0);
  for (size_t i = 0; i < raw.size(); i++) {
    if (!out.empty() && i + 1 < raw.size() && D2(out.back(), raw[i]) < 150 && fabs(out.back().z - raw[i].z) < 60) continue;
    out.push_back(raw[i]);
  }
  if (!out.empty() && D2(out[0], from) < 100) out.erase(out.begin());
  return !out.empty();
}
static bool Teleport(V3 v) { return SafeTeleport(v, "unstuck"); }
static int g_nextId = 1;

static void Finish(const char* st, const std::string& det)
{
  if (g.kind == G_GOTO || g.kind == G_HOLDMOVE) SetMove(0, 0);
  if (g.kind == G_FIRE) Call("pawn", "OnFireReleased", "");
  if (g.kind == G_HOLD) HoldNat(1);
  g.state = st; g.detail = det; Log("goal %d %s %s", g.id, st, det.c_str()); g.kind = G_NONE;
}

static std::string StartGoto(const std::vector<V3>& pts, double tol, double timeout, const std::string& seg)
{
  if (Dead()) { Log("BENCH WARN goto refused seg=%s: pawn dead (GetHP 0)", seg.c_str()); return "err pawn dead"; }
  if (g.kind != G_NONE) Finish("aborted", "replaced");
  g = Goal(); g.kind = G_GOTO; g.id = g_nextId++; g.pts = pts; g.tol = tol; g.timeout = timeout; g.seg = seg; g_seg = seg; g.t0 = GetTickCount64(); g.state = "running";
  Pos(g.lastPos); g.lastCheck = g.t0;
  Log("goal %d goto seg=%s pts=%zu first=%.0f,%.0f,%.0f tol=%.0f", g.id, seg.c_str(), pts.size(), pts[0].x, pts[0].y, pts[0].z, tol);
  return Fmt("goal %d running", g.id);
}

static bool NoActor(const std::string& a) { return a.find("ReturnValue = ") == std::string::npos || a.find("ReturnValue = null") != std::string::npos || a.find("ReturnValue = None") != std::string::npos; }
static std::string StartUse(V3 to, int holdMs, double timeout)
{
  if (Dead()) return "err pawn dead";
  if (g.kind != G_NONE) Finish("aborted", "replaced");
  g = Goal(); g.kind = G_USE; g.id = g_nextId++; g.state = "running"; g.dest = to; g.tol = holdMs; g.t0 = GetTickCount64(); g.until = g.t0 + (uint64_t)(timeout * 1000);
  V3 p; if (Pos(p)) LookAt(p, to);
  Log("goal %d use at %.0f,%.0f,%.0f hold=%d", g.id, to.x, to.y, to.z, holdMs);
  return Fmt("goal %d running", g.id);
}
static void TickUse()
{
  V3 p; if (!Pos(p)) { Finish("failed", "no pawn"); return; }
  LookAt(p, g.dest);
  std::string a = Call("pawn", "GetInteractedActor", "");
  uint64_t now = GetTickCount64();
  double d = sqrt((g.dest.x - p.x) * (g.dest.x - p.x) + (g.dest.y - p.y) * (g.dest.y - p.y) + (g.dest.z - p.z) * (g.dest.z - p.z));
  if (NoActor(a)) { if (now >= g.until) Finish("failed", Fmt("nothing to interact after %.1f s, target %.0f cm away", (now - g.t0) / 1000.0, d)); return; }
  size_t q = a.find("ReturnValue = "); std::string an = a.substr(q + 14); while (!an.empty() && (an.back() == '\n' || an.back() == '\r')) an.pop_back();
  if (g.tol > 0) {
    int id = g.id; uint64_t ms = (uint64_t)g.tol;
    g = Goal(); g.kind = G_HOLD; g.id = id; g.state = "running"; g.detail = "hold " + an; g.until = now + ms;
    int r = HoldNat(0); Log("goal %d use hold %llu ms actor=%s d=%.0f rc=0x%x", id, (unsigned long long)ms, an.c_str(), d, r);
    return;
  }
  std::string r = Tap();
  Finish(r.rfind("tap rc=0x0 ", 0) == 0 ? "done" : "failed", Fmt("%s d=%.0f", r.c_str(), d));
}
static void TickGoto()
{
  uint64_t now = GetTickCount64();
  if (TickSettle()) { g.lastCheck = now; Pos(g.lastPos); return; }
  if (FallGuard()) { g.lastCheck = now; return; }
  V3 p; if (!Pos(p)) { Finish("failed", "no pawn"); return; }
  if ((now - g.t0) / 1000.0 > g.timeout) { Finish("failed", Fmt("timeout at %.0f,%.0f,%.0f pt %zu/%zu", p.x, p.y, p.z, g.idx + 1, g.pts.size())); return; }
  V3 t = g.pts[g.idx];
  double d = D2(p, t);
  double tol = g.idx + 1 < g.pts.size() ? (std::max)(80.0, g.tol * 0.6) : g.tol;
  if (d < tol) {
    if (++g.idx >= g.pts.size()) { Finish("done", Fmt("at %.0f,%.0f,%.0f d=%.0f tps=%d jumps=%d secs=%.1f", p.x, p.y, p.z, d, g.tps, g.jumps, (now - g.t0) / 1000.0)); return; }
    t = g.pts[g.idx]; d = D2(p, t); g.best = d; g.stuck = 0;
  }
  double yaw = atan2(t.y - p.y, t.x - p.x) * 57.29578;
  SetRot(0, yaw);
  if (g.until && now < g.until) return;
  g.until = 0;
  SetMove(1, 0);
  if (now - g.lastCheck >= 1500) {
    bool prog = d < g.best - 60;
    if (d < g.best) g.best = d;
    if (!prog) {
      g.stuck++;
      if (g.nav && g.stuck == 3 && g.replans < 3) {
        std::vector<V3> np; bool part = false;
        if (NavPath(p, g.dest, np, part)) { g.pts = np; g.idx = 0; g.best = 1e18; g.replans++; g.stuck = 0; Log("goal %d seg=%s replan %d pts=%zu partial=%d", g.id, g.seg.c_str(), g.replans, np.size(), part ? 1 : 0); g.lastPos = p; g.lastCheck = now; return; }
      }
      if (g.stuck == 1 || g.stuck == 3) { Call("pawn", "OnJumpPressed", ""); g.jumps++; Log("goal %d seg=%s stuck %d at %.0f,%.0f,%.0f -> jump", g.id, g.seg.c_str(), g.stuck, p.x, p.y, p.z); }
      else if (g.stuck == 2) { SetMove(0.2, 1); g.until = now + 1500; Log("goal %d seg=%s stuck %d -> strafe right", g.id, g.seg.c_str(), g.stuck); }
      else if (g.stuck == 4) { SetMove(-0.5, -1); g.until = now + 2500; Log("goal %d seg=%s stuck %d -> back left", g.id, g.seg.c_str(), g.stuck); }
      else if (g.stuck == 5) { SetMove(-0.5, 1); g.until = now + 3500; Log("goal %d seg=%s stuck %d -> back right", g.id, g.seg.c_str(), g.stuck); }
      else if (g.stuck >= 6) {
        if (g.nav && g.tps >= 2) { Finish("failed", Fmt("stuck at %.0f,%.0f,%.0f pt %zu/%zu after %d unstuck teleports", p.x, p.y, p.z, g.idx + 1, g.pts.size(), g.tps)); return; }
        bool ok = false;
        if (g.allowTp) { double k = (std::min)(1.0, 300.0 / (std::max)(1.0, d)); V3 n{ p.x + (t.x - p.x) * k, p.y + (t.y - p.y) * k, (std::max)(p.z, t.z) }; ok = Teleport(n); if (!ok) ok = Teleport(t); }
        g.tps += ok ? 1 : 0; g.stuck = 0;
        if (!ok) { Finish("failed", Fmt("stuck at %.0f,%.0f,%.0f pt %zu/%zu, no safe teleport", p.x, p.y, p.z, g.idx + 1, g.pts.size())); return; }
      }
    } else g.stuck = 0;
    g.lastPos = p; g.lastCheck = now;
  }
}

struct Rec {
  bool on = false; FILE* fp = nullptr; uint64_t t0 = 0, lastPosT = 0; std::string path;
  V3 lastP{}; double lastYaw = 0; int mode = -1, cmode = -1, crouch = -1; std::string equip; double hp = -1;
  bool keyDown[256] = {}; uint64_t keyT[256] = {}; std::string fActor, fComp; V3 fActorPos{}; V3 fP{}; double fYaw = 0, fPitch = 0;
  int n = 0; bool hotPrev = false, markPrev = false; int marks = 0;
};
static Rec R;
static std::string JsonSafe(std::string v) { for (auto& c : v) if (c == '"' || c == '\\' || c == '\n' || c == '\r' || c == '\t') c = ' '; while (!v.empty() && v.back() == ' ') v.pop_back(); return v; }
static void RecWrite(const char* ev, const std::string& body)
{
  if (!R.fp) return;
  fprintf(R.fp, "{\"t\":%llu,\"ev\":\"%s\"%s%s}\n", (unsigned long long)(GetTickCount64() - R.t0), ev, body.empty() ? "" : ",", body.c_str());
  fflush(R.fp); R.n++;
}
static std::string RetObj(const std::string& r)
{
  size_t q = r.find("ReturnValue = "); if (q == std::string::npos) return ""; std::string v = r.substr(q + 14);
  size_t e = v.find('\n'); if (e != std::string::npos) v = v.substr(0, e);
  if (v.rfind("0x", 0) != 0) return ""; return JsonSafe(v);
}
static bool Focused() { DWORD pid = 0; HWND w = GetForegroundWindow(); if (!w) return false; GetWindowThreadProcessId(w, &pid); return pid == GetCurrentProcessId(); }
static std::string PosJ(V3 p) { return Fmt("[%.1f,%.1f,%.1f]", p.x, p.y, p.z); }
static int GetInt(const char* path) { uintptr_t o = Find("pawn"); char out[256] = {0}; if (!o) return -1; api->get(o, path, out, sizeof out); if (!strcmp(out, "true")) return 1; if (!strcmp(out, "false")) return 0; return (out[0] >= '0' && out[0] <= '9') ? atoi(out) : -1; }
static std::string EquipNow() { std::string eq = Call("pawn", "GetMainHandEquipType", ""); size_t q = eq.find("ReturnValue = "); if (q == std::string::npos) return "?"; std::string e = eq.substr(q + 14); size_t z = e.find('\n'); if (z != std::string::npos) e = e.substr(0, z); return JsonSafe(e); }
static double VelZ() { uintptr_t o = Find("pawn"); char out[256] = {0}; if (!o) return 0; api->get(o, "CharacterMovement.Velocity", out, sizeof out); return ParseNum(out, "Z="); }
static void Snapshot(const char* ev)
{
  V3 p{}, r{}; Pos(p); Rot(r);
  RecWrite(ev, Fmt("\"p\":%s,\"pitch\":%.1f,\"yaw\":%.1f,\"mode\":%d,\"cmode\":%d,\"crouch\":%d,\"equip\":\"%s\",\"seg\":\"%s\"", PosJ(p).c_str(), r.x, r.y, GetInt("CharacterMovement.MovementMode"), GetInt("CharacterMovement.CustomMovementMode"), GetInt("bIsCrouched"), EquipNow().c_str(), g_seg.c_str()));
}
static std::string RecStart(const std::string& name)
{
  if (R.on) return "err already recording " + R.path;
  std::string dir = S2Path("bench\\rec"); CreateDirectoryA(dir.c_str(), nullptr);
  dir += Fmt("\\%d", g_port); CreateDirectoryA(dir.c_str(), nullptr);
  SYSTEMTIME t; GetLocalTime(&t);
  R = Rec(); R.path = dir + Fmt("\\rec_%04d%02d%02d_%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond) + (name.empty() ? "" : "_" + name) + ".jsonl";
  R.fp = fopen(R.path.c_str(), "wb"); if (!R.fp) return "err open " + R.path;
  R.on = true; R.t0 = GetTickCount64();
  RecWrite("start", Fmt("\"port\":%d,\"role\":\"%s\",\"name\":\"%s\",\"ver\":1", g_port, (g_port == 27063 || g_port == 27065) ? "host" : "client", JsonSafe(name).c_str()));
  Snapshot("pos");
  Log("rec start %s", R.path.c_str()); MessageBeep(MB_OK);
  return "rec " + R.path;
}
static std::string RecStop()
{
  if (!R.on) return "err not recording";
  Snapshot("pos"); RecWrite("stop", Fmt("\"events\":%d", R.n));
  fclose(R.fp); R.fp = nullptr; R.on = false; Log("rec stop %s events=%d", R.path.c_str(), R.n);
  MessageBeep(MB_ICONHAND);
  return Fmt("stopped %s events=%d", R.path.c_str(), R.n);
}
static const struct { int vk; const char* name; } kKeys[] = {
  { 'F', "F" }, { VK_SPACE, "Space" }, { 'C', "C" }, { VK_LCONTROL, "LCtrl" }, { 'Z', "Z" }, { 'G', "G" }, { 'V', "V" }, { 'R', "R" }, { 'Q', "Q" }, { 'E', "E" },
  { '1', "1" }, { '2', "2" }, { '3', "3" }, { '4', "4" }, { '5', "5" }, { '6', "6" }, { '7', "7" }, { '8', "8" }, { '9', "9" },
  { VK_LBUTTON, "LMB" }, { VK_RBUTTON, "RMB" }, { VK_TAB, "Tab" }, { VK_RETURN, "Enter" }, { VK_ESCAPE, "Esc" }, { 'I', "I" }, { 'H', "H" }, { 'M', "M" }, { VK_LSHIFT, "LShift" },
  { 'W', "W" }, { 'A', "A" }, { 'S', "S" }, { 'D', "D" }, { VK_UP, "Up" }, { VK_DOWN, "Down" }, { VK_F5, "F5" },
};
static void RecKeys()
{
  bool foc = Focused();
  for (auto& k : kKeys) {
    bool down = foc && (GetAsyncKeyState(k.vk) & 0x8000) != 0;
    if (down == R.keyDown[k.vk & 0xff]) continue;
    R.keyDown[k.vk & 0xff] = down; uint64_t now = GetTickCount64();
    bool move = k.vk == 'W' || k.vk == 'A' || k.vk == 'S' || k.vk == 'D' || k.vk == VK_LSHIFT;
    if (down) {
      R.keyT[k.vk & 0xff] = now;
      if (k.vk == 'F') {
        R.fActor = RetObj(Call("pawn", "GetInteractedActor", "")); R.fComp = RetObj(Call("pawn", "GetInteractedComponent", ""));
        R.fActorPos = V3{}; if (!R.fActor.empty()) { uintptr_t a = (uintptr_t)_strtoui64(R.fActor.c_str(), nullptr, 16); ParseV(CallO(a, "K2_GetActorLocation", ""), R.fActorPos); }
        V3 r{}; Pos(R.fP); Rot(r); R.fPitch = r.x; R.fYaw = r.y;
      }
      if (!move) RecWrite("key", Fmt("\"k\":\"%s\",\"down\":1", k.name));
    } else {
      uint64_t held = now - R.keyT[k.vk & 0xff];
      if (!move) RecWrite("key", Fmt("\"k\":\"%s\",\"down\":0,\"ms\":%llu", k.name, (unsigned long long)held));
      if (k.vk == 'F') {
        RecWrite("interact", Fmt("\"act\":\"%s\",\"ms\":%llu,\"actor\":\"%s\",\"comp\":\"%s\",\"apos\":%s,\"p\":%s,\"pitch\":%.1f,\"yaw\":%.1f,\"seg\":\"%s\"",
          held < 300 ? "tap" : "hold", (unsigned long long)held, R.fActor.c_str(), R.fComp.c_str(), PosJ(R.fActorPos).c_str(), PosJ(R.fP).c_str(), R.fPitch, R.fYaw, g_seg.c_str()));
        Snapshot("after_interact");
      }
    }
  }
}
static void RecMode()
{
  int mode = GetInt("CharacterMovement.MovementMode"), cmode = GetInt("CharacterMovement.CustomMovementMode");
  if (mode != R.mode || cmode != R.cmode) {
    V3 p{}, r{}; Pos(p); Rot(r);
    double vz = VelZ();
    RecWrite("mode", Fmt("\"from\":%d,\"to\":%d,\"cfrom\":%d,\"cto\":%d,\"vz\":%.0f,\"p\":%s,\"yaw\":%.1f", R.mode, mode, R.cmode, cmode, isnan(vz) ? 0.0 : vz, PosJ(p).c_str(), r.y));
    if (R.mode == 1 && mode == 3 && !isnan(vz) && vz > 150) RecWrite("jump", Fmt("\"p\":%s,\"yaw\":%.1f", PosJ(p).c_str(), r.y));
    R.mode = mode; R.cmode = cmode;
  }
}
static uint64_t g_recSampleT;
static void RecSample()
{
  uint64_t now = GetTickCount64();
  if (now - g_recSampleT < 100) return; g_recSampleT = now;
  int mode = R.mode, cr = GetInt("bIsCrouched");
  V3 p{}, r{}; Pos(p); Rot(r);
  if (cr != R.crouch) { if (R.crouch >= 0) RecWrite("crouch", Fmt("\"on\":%d,\"p\":%s", cr, PosJ(p).c_str())); R.crouch = cr; }
  std::string eq = EquipNow(); if (eq != R.equip) { if (!R.equip.empty()) RecWrite("equip", Fmt("\"from\":\"%s\",\"to\":\"%s\"", R.equip.c_str(), eq.c_str())); R.equip = eq; }
  std::string hs = Call("pawn", "GetHP", ""); double h = ParseNum(hs, "ReturnValue");
  if (!isnan(h) && fabs(h - R.hp) > 2) { if (R.hp >= 0) RecWrite("hp", Fmt("\"from\":%.1f,\"to\":%.1f", R.hp, h)); R.hp = h; }
  double yd = fabs(r.y - R.lastYaw); if (yd > 180) yd = 360 - yd;
  if (D2(p, R.lastP) > 100 || fabs(p.z - R.lastP.z) > 60 || yd > 25 || now - R.lastPosT > 2000) {
    RecWrite("pos", Fmt("\"p\":%s,\"pitch\":%.1f,\"yaw\":%.1f,\"mode\":%d", PosJ(p).c_str(), r.x, r.y, mode));
    R.lastP = p; R.lastYaw = r.y; R.lastPosT = now;
  }
}
static void RecHotkey()
{
  bool hot = Focused() && (GetAsyncKeyState(VK_F7) & 0x8000);
  if (hot && !R.hotPrev) { std::string r = R.on ? RecStop() : RecStart(""); api->log("nbot %s", r.c_str()); }
  R.hotPrev = hot;
  bool mk = R.on && Focused() && (GetAsyncKeyState(VK_F8) & 0x8000);
  if (mk && !R.markPrev) { R.marks++; RecWrite("mark", Fmt("\"text\":\"seg%d\"", R.marks)); Snapshot("pos"); MessageBeep(MB_ICONASTERISK); }
  R.markPrev = mk;
}

static std::vector<std::string> Split(const std::string& s)
{
  std::vector<std::string> r; size_t i = 0;
  while (i < s.size()) { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++; size_t j = i; while (j < s.size() && !(s[j] == ' ' || s[j] == '\t' || s[j] == '\r' || s[j] == '\n')) j++; if (j > i) r.push_back(s.substr(i, j - i)); i = j; }
  return r;
}
static std::string Rest(const std::string& s, int n)
{
  size_t i = 0; for (int k = 0; k < n; k++) { while (i < s.size() && s[i] == ' ') i++; while (i < s.size() && s[i] != ' ') i++; }
  while (i < s.size() && s[i] == ' ') i++; return s.substr(i);
}
static bool ParsePts(const std::string& s, std::vector<V3>& pts)
{
  size_t i = 0;
  while (i < s.size()) {
    size_t j = s.find(';', i); if (j == std::string::npos) j = s.size();
    std::string p = s.substr(i, j - i); V3 v;
    if (sscanf(p.c_str(), "%lf,%lf,%lf", &v.x, &v.y, &v.z) == 3) pts.push_back(v);
    i = j + 1;
  }
  return !pts.empty();
}

static std::string StateJson()
{
  V3 p{}, r{}; bool hp = Pos(p); Rot(r);
  std::string hps = Call("pawn", "GetHP", ""), eq = Call("pawn", "GetMainHandEquipType", "");
  double h = ParseNum(hps, "ReturnValue");
  size_t q = eq.find("ReturnValue = "); std::string e = q == std::string::npos ? "?" : eq.substr(q + 14);
  for (auto& c : e) if (c == '"' || c == '\n' || c == '\r') c = ' ';
  return Fmt("{\"pawn\":%s,\"pos\":[%.1f,%.1f,%.1f],\"pitch\":%.1f,\"yaw\":%.1f,\"hp\":%.2f,\"equip\":\"%s\",\"goal\":{\"id\":%d,\"state\":\"%s\",\"detail\":\"%s\"}}",
    hp ? "true" : "false", p.x, p.y, p.z, r.x, r.y, isnan(h) ? -1.0 : h, e.c_str(), g.id, g.state.c_str(), g.detail.c_str());
}

static uintptr_t ClassObj(const char* cls);
static std::string SceneCmd(const std::string& line);
static std::string PlayCmd(const std::vector<std::string>& t, const std::string& line);
static std::string ImmuneCmd(const std::vector<std::string>& t);
static void TickImmune();
static std::string CamCmd(const std::vector<std::string>& t);
static void TickCam();
static std::string Exec(const std::string& line)
{
  auto t = Split(line); if (t.empty()) return "err empty";
  const std::string& c = t[0];
  if (c == "ping") return Fmt("pong port=%d tick=%llu", g_port, (unsigned long long)g_tick);
  if (c == "state") return StateJson();
  if (c == "status") return Fmt("{\"id\":%d,\"state\":\"%s\",\"detail\":\"%s\",\"kind\":%d}", g.id, g.state.c_str(), g.detail.c_str(), (int)g.kind);
  if (c == "stop") { if (g.kind != G_NONE) Finish("aborted", "stop"); SetMove(0, 0); return "stopped"; }
  if (c == "move" && t.size() >= 3) { SetMove(atof(t[1].c_str()), atof(t[2].c_str())); return "ok"; }
  if (c == "holdmove" && t.size() >= 4) {
    if (Dead()) return "err pawn dead";
    if (g.kind != G_NONE) Finish("aborted", "replaced");
    g = Goal(); g.kind = G_HOLDMOVE; g.id = g_nextId++; g.state = "running"; g.until = GetTickCount64() + (uint64_t)atoi(t[3].c_str());
    SetMove(atof(t[1].c_str()), atof(t[2].c_str())); return Fmt("goal %d running", g.id);
  }
  if (c == "rot" && t.size() >= 3) { SetRot(atof(t[1].c_str()), atof(t[2].c_str())); return "ok"; }
  if (c == "look" && t.size() >= 4) { V3 p; if (!Pos(p)) return "err no pawn"; LookAt(p, V3{ atof(t[1].c_str()), atof(t[2].c_str()), atof(t[3].c_str()) }); return "ok"; }
  if (c == "goto" && t.size() >= 2) {
    std::vector<V3> pts; if (!ParsePts(t[1], pts)) return "err pts";
    double tol = t.size() > 2 ? atof(t[2].c_str()) : 150, to = t.size() > 3 ? atof(t[3].c_str()) : 90;
    return StartGoto(pts, tol, to, t.size() > 4 ? t[4] : "-");
  }
  if (c == "tp" && t.size() >= 2) { std::vector<V3> pts; if (!ParsePts(t[1], pts)) return "err pts"; return SafeTeleport(pts[0], "cmd") ? "ok" : "err tp refused"; }
  if (c == "ground") { Floor f{}; bool og = OnGround(&f); V3 p{}; Pos(p); return Fmt("{\"ground\":%s,\"mode\":%d,\"hit\":%d,\"walk\":%d,\"dist\":%.1f,\"pos\":[%.1f,%.1f,%.1f],\"crumbs\":%zu,\"settling\":%d}", og ? "true" : "false", MoveMode(), f.hit ? 1 : 0, f.walk ? 1 : 0, f.dist, p.x, p.y, p.z, g_crumbs.size(), g_set.on ? 1 : 0); }
  if (c == "floor" && t.size() >= 2) { std::vector<V3> pts; if (!ParsePts(t[1], pts)) return "err pts"; Floor f{}; bool ok = FloorAt(pts[0], t.size() > 2 ? (float)atof(t[2].c_str()) : 400.f, f); return Fmt("{\"ok\":%d,\"hit\":%d,\"walk\":%d,\"dist\":%.1f,\"impact\":[%.1f,%.1f,%.1f]}", ok ? 1 : 0, f.hit ? 1 : 0, f.walk ? 1 : 0, f.dist, f.imp.x, f.imp.y, f.imp.z); }
  if (c == "rawget" && t.size() >= 3) { uintptr_t o = Find(t[1].c_str()); char out[1024] = {0}; int r = api->get(o, t[2].c_str(), out, sizeof out); return Fmt("obj=0x%llx r=%d [%s]", (unsigned long long)o, r, out); }
  if (c == "findobj" && t.size() >= 2) return Fmt("0x%llx", (unsigned long long)Find(t[1].c_str()));
  if (c == "path" && t.size() >= 2) {
    std::vector<V3> pts; if (!ParsePts(t[1], pts)) return "err pts"; V3 p{}; if (!Pos(p)) return "err no pawn";
    std::vector<V3> np; bool part = false; if (!NavPath(p, pts[0], np, part)) return "err no path";
    std::string o = Fmt("{\"partial\":%s,\"n\":%zu,\"pts\":\"", part ? "true" : "false", np.size());
    for (size_t i = 0; i < np.size(); i++) o += Fmt("%s%.0f,%.0f,%.0f", i ? ";" : "", np[i].x, np[i].y, np[i].z);
    return o + "\"}";
  }
  if (c == "nav" && t.size() >= 2) {
    std::vector<V3> pts; if (!ParsePts(t[1], pts)) return "err pts"; V3 p{}; if (!Pos(p)) return "err no pawn";
    std::vector<V3> np; bool part = false; if (!NavPath(p, pts[0], np, part)) return "err no path";
    if (part) Log("BENCH WARN partial path seg=%s to=%.0f,%.0f,%.0f end=%.0f,%.0f,%.0f", t.size() > 4 ? t[4].c_str() : "-", pts[0].x, pts[0].y, pts[0].z, np.back().x, np.back().y, np.back().z);
    if (part && D2(np.back(), pts[0]) > 200) np.push_back(pts[0]);
    std::string r = StartGoto(np, t.size() > 2 ? atof(t[2].c_str()) : 150, t.size() > 3 ? atof(t[3].c_str()) : 90, t.size() > 4 ? t[4] : "-");
    g.nav = true; g.dest = pts[0]; return r + Fmt(" pts=%zu partial=%d", np.size(), part ? 1 : 0);
  }
  if (c == "rec" && t.size() >= 2) {
    if (t[1] == "start") return RecStart(t.size() > 2 ? t[2] : "");
    if (t[1] == "stop") return RecStop();
    if (t[1] == "status") return R.on ? Fmt("recording %s events=%d", R.path.c_str(), R.n) : "idle";
    if (t[1] == "mark") { RecWrite("mark", Fmt("\"text\":\"%s\"", JsonSafe(Rest(line, 2)).c_str())); Snapshot("pos"); return "ok"; }
  }
  if (c == "perf") {
    LARGE_INTEGER f, a0, a1; QueryPerformanceFrequency(&f); std::string o; V3 v{};
    QueryPerformanceCounter(&a0); for (int i = 0; i < 20; i++) Find("pawn"); QueryPerformanceCounter(&a1); o += Fmt("find %.3fms ", (a1.QuadPart - a0.QuadPart) * 1000.0 / f.QuadPart / 20);
    QueryPerformanceCounter(&a0); for (int i = 0; i < 20; i++) GetInt("CharacterMovement.MovementMode"); QueryPerformanceCounter(&a1); o += Fmt("getint %.3fms ", (a1.QuadPart - a0.QuadPart) * 1000.0 / f.QuadPart / 20);
    QueryPerformanceCounter(&a0); for (int i = 0; i < 20; i++) Pos(v); QueryPerformanceCounter(&a1); o += Fmt("pos %.3fms ", (a1.QuadPart - a0.QuadPart) * 1000.0 / f.QuadPart / 20);
    QueryPerformanceCounter(&a0); for (int i = 0; i < 20; i++) EquipNow(); QueryPerformanceCounter(&a1); o += Fmt("equip %.3fms ", (a1.QuadPart - a0.QuadPart) * 1000.0 / f.QuadPart / 20);
    QueryPerformanceCounter(&a0); for (int i = 0; i < 20; i++) Focused(); QueryPerformanceCounter(&a1); o += Fmt("focus %.3fms ", (a1.QuadPart - a0.QuadPart) * 1000.0 / f.QuadPart / 20);
    QueryPerformanceCounter(&a0); for (int i = 0; i < 20; i++) GetAsyncKeyState('F'); QueryPerformanceCounter(&a1); o += Fmt("key %.3fms", (a1.QuadPart - a0.QuadPart) * 1000.0 / f.QuadPart / 20);
    return o;
  }
  if (c == "seg" && t.size() >= 2) { g_seg = t[1]; return "ok"; }
  if (c == "iact") return Call("pawn", "GetInteractedActor", "") + " | " + Call("pawn", "GetInteractedComponent", "");
  if (c == "tap") return Tap();
  if (c == "hpguard" && t.size() >= 2) { g_hpg = atof(t[1].c_str()); g_hpgT = 0; Log("hpguard set %.0f", g_hpg); return Fmt("hpguard %.0f raised=%d", g_hpg, g_hpgN); }
  if (c == "hpmin") {
    if (t.size() >= 2 && t[1] == "reset") { g_hpMin = 1e9; g_hpS = 0; return "hpmin reset"; }
    double lim = t.size() >= 2 ? atof(t[1].c_str()) : 0; std::string o = Fmt("hpmin %.1f samples=%d raised=%d", g_hpS ? g_hpMin : -1.0, g_hpS, g_hpgN);
    Log("%s limit %.0f", o.c_str(), lim);
    return g_hpS == 0 || g_hpMin <= lim ? "err " + o + Fmt(" (limit %.0f)", lim) : o;
  }
  if (c == "immune") return ImmuneCmd(t);
  if (c == "cam") return CamCmd(t);
  if (c == "hold" && t.size() >= 2) {
    if (g.kind != G_NONE) Finish("aborted", "replaced");
    g = Goal(); g.kind = G_HOLD; g.id = g_nextId++; g.state = "running"; g.until = GetTickCount64() + (uint64_t)atoi(t[1].c_str());
    int r = HoldNat(0); return Fmt("hold start rc=0x%x goal %d", r, g.id);
  }
  if (c == "jump") return Call("pawn", "OnJumpPressed", "");
  if (c == "crouch" && t.size() >= 2) return Call("pawn", t[1] == "1" ? "OnCrouchPressed" : "OnCrouchReleased", "");
  if (c == "equip" && t.size() >= 2) { uintptr_t pw = Find("pawn"); if (!pw) return "err no pawn"; if (!Nat().equip) return "err equip native not found by signature"; return Cmd(Fmt("xcall 0x%llx \"[0x%llx+0x650]\" %d", (unsigned long long)Nat().equip, (unsigned long long)pw, atoi(t[1].c_str()))); }
  if (c == "melee") return Call("pawn", "OnMeleeAttack", "");
  if (c == "aim" && t.size() >= 2) return Call("pawn", t[1] == "1" ? "OnAimPressed" : "OnAimReleased", "");
  if (c == "reload") return Call("pawn", "Reload", "");
  if (c == "use" && t.size() >= 4) return StartUse(V3{ atof(t[1].c_str()), atof(t[2].c_str()), atof(t[3].c_str()) }, t.size() > 4 ? atoi(t[4].c_str()) : 0, t.size() > 5 ? atof(t[5].c_str()) : 3);
  if (c == "use") return Tap();
  if (c == "heal") {
    uintptr_t rk = ClassObj("CustomConsoleManagerRK"); if (!rk) return "err no manager";
    std::vector<std::string> sids(t.begin() + 1, t.end()); if (sids.empty()) sids = { "MedkitHealing3", "MedkitBleeding2" };
    std::string o; for (auto& s : sids) o += s + "=" + CallO(rk, "XApplyEffectOnPlayer", s.c_str()).substr(0, 20) + " ";
    return "heal " + o;
  }
  if (c == "fire" && t.size() >= 2) {
    if (g.kind != G_NONE) Finish("aborted", "replaced");
    g = Goal(); g.kind = G_FIRE; g.id = g_nextId++; g.state = "running"; g.until = GetTickCount64() + (uint64_t)atoi(t[1].c_str());
    return Call("pawn", "OnFirePressed", "") + Fmt(" goal %d", g.id);
  }
  if (c == "call" && t.size() >= 3) return Call(t[1].c_str(), t[2].c_str(), Rest(line, 3).c_str());
  if (c == "s2" && t.size() >= 2) return Cmd(Rest(line, 1));
  if (c == "scene") return SceneCmd(line);
  if (c == "play" || c == "quest") return PlayCmd(t, line);
  return "err unknown " + c;
}

#include "nbot_scenerun.h"
#include "nbot_playrun.h"

static std::string PlayCmd(const std::vector<std::string>& t, const std::string& line)
{
  if (t[0] == "quest") return QuestDump(t.size() > 1 ? t[1] : "");
  if (t.size() >= 2 && t[1] == "status") return PlayJson();
  if (t.size() >= 2 && t[1] == "stop") { if (P.on) PlayEnd("stopped", "stop"); return PlayJson(); }
  return PlayStart(t);
}
struct Immune { bool on = false; std::string sid = "StrelokImmune"; uint64_t per = 2000, next = 0; int applied = 0, forced = 0, lastBits = -1; };
static Immune g_imm;
static int RecBits(uintptr_t& rec)
{
  uintptr_t pw = Find("pawn"); rec = 0; if (!pw) return -1;
  __try { rec = *(uintptr_t*)(pw + 0x650); if (!rec) return -1; return *(uint16_t*)(rec + 0x260); }
  __except (EXCEPTION_EXECUTE_HANDLER) { rec = 0; return -1; }
}
static bool OrBits(uintptr_t rec, uint16_t m)
{
  __try { *(uint16_t*)(rec + 0x260) |= m; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static std::string ImmuneCmd(const std::vector<std::string>& t)
{
  if (t.size() >= 2 && t[1] == "off") { g_imm.on = false; Log("immune off applied=%d forced=%d", g_imm.applied, g_imm.forced); return Fmt("immune off applied=%d forced=%d", g_imm.applied, g_imm.forced); }
  if (t.size() >= 2 && t[1] == "on") {
    g_imm.on = true; g_imm.next = 0;
    if (t.size() >= 3) g_imm.sid = t[2];
    if (t.size() >= 4) g_imm.per = (uint64_t)atoi(t[3].c_str());
    TickImmune();
    uintptr_t rec; int b = RecBits(rec);
    Log("shortcut: immune on effect=%s every %llu ms (EIgnoreDamageType bits at [pawn+0x650]+0x260 = 0x%x); tests only, upgrade when the bot fights or sneaks past the bunker soldiers", g_imm.sid.c_str(), (unsigned long long)g_imm.per, b);
    if (b < 0 || (b & 6) != 6) return Fmt("err immune bits 0x%x after %s (applied=%d forced=%d)", b, g_imm.sid.c_str(), g_imm.applied, g_imm.forced);
    return Fmt("immune on %s bits=0x%x applied=%d forced=%d", g_imm.sid.c_str(), b, g_imm.applied, g_imm.forced);
  }
  uintptr_t rec; int b = RecBits(rec);
  return Fmt("immune %s %s bits=0x%x applied=%d forced=%d", g_imm.on ? "on" : "off", g_imm.sid.c_str(), b, g_imm.applied, g_imm.forced);
}
static void TickImmune()
{
  uint64_t now = GetTickCount64(); if (!g_imm.on || now < g_imm.next) return; g_imm.next = now + g_imm.per;
  uintptr_t rk = ClassObj("CustomConsoleManagerRK"); if (!rk) return;
  std::string k = CallO(rk, "XApplyEffectOnPlayer", g_imm.sid.c_str()); if (k.rfind("ok", 0) == 0) g_imm.applied++;
  uintptr_t rec; int b = RecBits(rec);
  if (b >= 0 && (b & 6) != 6 && rec && OrBits(rec, 6)) { g_imm.forced++; if (g_imm.forced <= 5 || g_imm.forced % 50 == 0) Log("immune forced #%d bits 0x%x -> 0x%x (effect %s gave %s)", g_imm.forced, b, b | 6, g_imm.sid.c_str(), k.substr(0, 30).c_str()); b |= 6; }
  if (b != g_imm.lastBits) { Log("immune bits 0x%x applied=%d forced=%d", b, g_imm.applied, g_imm.forced); g_imm.lastBits = b; }
}
struct Cam { bool on = false; uintptr_t cam = 0, tgt = 0; double dist = 250, h = 40, yaw = 0, aimz = 20, wy = NAN; int n = 0; V3 at{};
  bool detached = false, play = false, orbit = false, orbitSelf = false; uintptr_t orbitTgt = 0; double orbitR = 0, orbitH = 0, orbitDps = 0; ULONGLONG t0 = 0; std::vector<cine::Key> keys; };
static Cam g_cam;
static uintptr_t RetAddr(const std::string& s)
{
  size_t k = s.find("ReturnValue = 0x"); return k == std::string::npos ? 0 : (uintptr_t)_strtoui64(s.c_str() + k + 14, nullptr, 16);
}
static bool CamPlace()
{
  V3 tp{}; if (!Valid(g_cam.tgt) || !ActorPos(g_cam.tgt, tp)) return false;
  if (isnan(g_cam.wy)) { double ty = ParseNum(CallO(g_cam.tgt, "K2_GetActorRotation", ""), "Yaw="); if (isnan(ty)) return false; g_cam.wy = ty + g_cam.yaw; }
  double a = g_cam.wy / 57.29577951308232;
  V3 c{ tp.x + cos(a) * g_cam.dist, tp.y + sin(a) * g_cam.dist, tp.z + g_cam.h };
  nsc::Rot lr = nsc::look_rot(c.x, c.y, c.z, tp.x, tp.y, tp.z + g_cam.aimz);
  CallO(g_cam.cam, "K2_SetActorLocation", Fmt("%.1f,%.1f,%.1f false false", c.x, c.y, c.z).c_str());
  CallO(g_cam.cam, "K2_SetActorRotation", Fmt("%.2f,%.2f,0 false", lr.pitch, lr.yaw).c_str());
  g_cam.at = c; g_cam.n++; return true;
}
static std::string CamOff(const char* why)
{
  if (!g_cam.cam) return "cam off";
  Call("pc", "SetViewTargetWithBlend", "pawn 0 VTBlend_Linear 0 false");
  if (Valid(g_cam.cam)) CallO(g_cam.cam, "K2_DestroyActor", "");
  Log("cam off (%s) placed=%d", why, g_cam.n);
  g_cam = Cam(); return Fmt("cam off %s", why);
}
static bool CamEnsure(V3 at)
{
  if (g_cam.cam && Valid(g_cam.cam)) return true;
  uintptr_t gs = Find("Default__GameplayStatics"); if (!gs) return false;
  std::string xf = Fmt("(Rotation=(X=0,Y=0,Z=0,W=1),Translation=(X=%.0f,Y=%.0f,Z=%.0f),Scale3D=(X=1,Y=1,Z=1))", at.x, at.y, at.z);
  uintptr_t c = RetAddr(CallO(gs, "BeginDeferredActorSpawnFromClass", ("pc /Script/Engine.CameraActor " + xf + " AlwaysSpawn pc MultiplyWithRoot").c_str()));
  if (!c) return false;
  CallO(gs, "FinishSpawningActor", Fmt("0x%llx %s MultiplyWithRoot", (unsigned long long)c, xf.c_str()).c_str());
  g_cam.cam = c; return true;
}
static void PlacePose(const cine::Pose& p)
{
  CallO(g_cam.cam, "K2_SetActorLocation", Fmt("%.1f,%.1f,%.1f false false", p.pos.x, p.pos.y, p.pos.z).c_str());
  CallO(g_cam.cam, "K2_SetActorRotation", Fmt("%.2f,%.2f,%.2f false", p.rot.pitch, p.rot.yaw, p.rot.roll).c_str());
  uintptr_t cc = RetAddr(CallO(g_cam.cam, "GetCameraComponent", ""));
  if (cc) CallO(cc, "SetFieldOfView", Fmt("%.2f", p.fov).c_str());
  g_cam.at = V3{p.pos.x, p.pos.y, p.pos.z}; g_cam.n++;
}
static bool ViewPose(cine::Pose& out)
{
  uintptr_t mgr = RetAddr(Call("pc", "GetPlayerCameraManager", ""));
  V3 loc{};
  if (!mgr || !ParseV(CallO(mgr, "GetCameraLocation", ""), loc)) return false;
  std::string rs = CallO(mgr, "GetCameraRotation", "");
  double p = ParseNum(rs, "Pitch="), y = ParseNum(rs, "Yaw="), r = ParseNum(rs, "Roll=");
  if (isnan(p) || isnan(y) || isnan(r)) return false;
  double fov = ParseNum(CallO(mgr, "GetFOVAngle", ""), "ReturnValue = ");
  out = cine::Pose{cine::Vec{loc.x, loc.y, loc.z}, cine::Rot{p, y, r}, isnan(fov) ? 90.0 : fov};
  return true;
}
static std::string CamShow(const char* what)
{
  Call("pc", "SetViewTargetWithBlend", Fmt("0x%llx 0 VTBlend_Linear 0 false", (unsigned long long)g_cam.cam).c_str());
  uintptr_t vt = RetAddr(Call("pc", "GetViewTarget", ""));
  Log("cam %s cam=0x%llx view=%s", what, (unsigned long long)g_cam.cam, vt == g_cam.cam ? "camera" : "NOT camera");
  return vt == g_cam.cam ? "" : "err view target did not switch to the camera";
}
static std::string CamHere()
{
  cine::Pose v{};
  if (!ViewPose(v)) return "err cam cannot read the view";
  if (!CamEnsure(V3{v.pos.x, v.pos.y, v.pos.z})) return "err camera spawn failed";
  g_cam.on = true; g_cam.detached = true; g_cam.play = false; g_cam.orbit = false;
  PlacePose(v);
  std::string e = CamShow("here"); if (!e.empty()) return e;
  return Fmt("cam here 0x%llx at=%.0f,%.0f,%.0f pitch=%.1f yaw=%.1f roll=%.1f fov=%.0f", (unsigned long long)g_cam.cam, v.pos.x, v.pos.y, v.pos.z, v.rot.pitch, v.rot.yaw, v.rot.roll, v.fov);
}
static std::string CamKey(const std::vector<std::string>& t)
{
  if (t.size() < 10) return "err cam key x y z pitch yaw roll fov secs";
  double a[8]; for (int i = 0; i < 8; i++) a[i] = atof(t[2 + i].c_str());
  g_cam.keys.push_back(cine::Key{cine::Vec{a[0], a[1], a[2]}, cine::Rot{a[3], a[4], a[5]}, a[6], a[7]});
  return Fmt("cam key %d at=%.0f,%.0f,%.0f secs=%.2f", (int)g_cam.keys.size(), a[0], a[1], a[2], a[7]);
}
static std::string CamKeyHere(const std::vector<std::string>& t)
{
  if (t.size() < 3) return "err cam keyhere secs";
  cine::Pose v{};
  if (!ViewPose(v)) return "err cam cannot read the view";
  g_cam.keys.push_back(cine::Key{v.pos, v.rot, v.fov, atof(t[2].c_str())});
  return Fmt("cam key %d at=%.0f,%.0f,%.0f secs=%.2f", (int)g_cam.keys.size(), v.pos.x, v.pos.y, v.pos.z, atof(t[2].c_str()));
}
static double KeysDur()
{
  double d = 0; for (size_t i = 1; i < g_cam.keys.size(); i++) d += std::max(g_cam.keys[i].secs, 1e-3);
  return d;
}
static void TickKeys()
{
  double el = (GetTickCount64() - g_cam.t0) / 1000.0;
  PlacePose(cine::sample(g_cam.keys, el));
  if (el >= KeysDur()) g_cam.play = false;
}
static std::string CamPlay()
{
  if (g_cam.keys.empty()) return "err cam play needs keys (cam key or cam keyhere first)";
  const cine::Vec k0 = g_cam.keys[0].pos;
  if (!CamEnsure(V3{k0.x, k0.y, k0.z})) return "err camera spawn failed";
  g_cam.on = true; g_cam.detached = true; g_cam.orbit = false; g_cam.play = true; g_cam.t0 = GetTickCount64();
  TickKeys();
  std::string e = CamShow("play"); if (!e.empty()) return e;
  return Fmt("cam play keys=%d secs=%.2f", (int)g_cam.keys.size(), KeysDur());
}
static void PlaceOrbit()
{
  V3 aim{};
  bool ok = g_cam.orbitSelf ? Pos(aim) : ActorPos(g_cam.orbitTgt, aim);
  if (!ok) { CamOff("orbit target lost"); return; }
  aim.z += g_cam.aimz;
  double el = (GetTickCount64() - g_cam.t0) / 1000.0;
  PlacePose(cine::orbit_pose(cine::Vec{aim.x, aim.y, aim.z}, g_cam.orbitR, g_cam.orbitH, g_cam.orbitDps * el));
}
static std::string CamOrbit(const std::vector<std::string>& t)
{
  if (t.size() < 3) return "err cam orbit <0xactor|self> [r=m] [h=cm] [dps=deg]";
  uintptr_t tgt = 0;
  if (t[2] != "self") {
    tgt = (uintptr_t)_strtoui64(t[2].c_str(), nullptr, 16);
    if (!Valid(tgt)) return "err cam orbit target " + t[2] + " is not a live object";
  }
  V3 aim{};
  if (!(tgt ? ActorPos(tgt, aim) : Pos(aim))) return "err cam orbit cannot read the target";
  if (!CamEnsure(aim)) return "err camera spawn failed";
  g_cam.orbitTgt = tgt; g_cam.orbitSelf = tgt == 0;
  g_cam.orbitR = atof(nsc::opt(t, "r", "3").c_str()) * 100; g_cam.orbitH = atof(nsc::opt(t, "h", "40").c_str()); g_cam.orbitDps = atof(nsc::opt(t, "dps", "20").c_str());
  g_cam.on = true; g_cam.detached = true; g_cam.play = false; g_cam.orbit = true; g_cam.t0 = GetTickCount64();
  PlaceOrbit();
  if (!g_cam.on) return "err cam orbit target lost";
  std::string e = CamShow("orbit"); if (!e.empty()) return e;
  return Fmt("cam orbit 0x%llx r=%.0f h=%.0f dps=%.1f", (unsigned long long)g_cam.cam, g_cam.orbitR, g_cam.orbitH, g_cam.orbitDps);
}
static std::string CamCmd(const std::vector<std::string>& t)
{
  if (t.size() < 2) return "err cam <0xactor> [dist=m] [h=cm] [yaw=deg from the actor facing] [wyaw=world deg] [aimz=cm] | cam here | cam orbit <0xactor|self> [r=m] [h=cm] [dps=deg] | cam key x y z pitch yaw roll fov secs | cam keyhere secs | cam play | cam clear | cam off | cam status";
  if (t[1] == "off") return CamOff("cmd");
  if (t[1] == "status") return Fmt("cam %s mode=%s keys=%d tgt=0x%llx cam=0x%llx placed=%d at=%.0f,%.0f,%.0f", g_cam.on ? "on" : "off", g_cam.detached ? (g_cam.orbit ? "orbit" : g_cam.play ? "play" : "here") : "follow", (int)g_cam.keys.size(), (unsigned long long)g_cam.tgt, (unsigned long long)g_cam.cam, g_cam.n, g_cam.at.x, g_cam.at.y, g_cam.at.z);
  if (t[1] == "here") return CamHere();
  if (t[1] == "orbit") return CamOrbit(t);
  if (t[1] == "key") return CamKey(t);
  if (t[1] == "keyhere") return CamKeyHere(t);
  if (t[1] == "play") return CamPlay();
  if (t[1] == "clear") { g_cam.keys.clear(); g_cam.play = false; return "cam keys cleared"; }
  uintptr_t tgt = (uintptr_t)_strtoui64(t[1].c_str(), nullptr, 16);
  if (!Valid(tgt)) return "err cam target " + t[1] + " is not a live object";
  g_cam.tgt = tgt; g_cam.detached = false; g_cam.play = false; g_cam.orbit = false;
  g_cam.dist = atof(nsc::opt(t, "dist", "2.5").c_str()) * 100; g_cam.h = atof(nsc::opt(t, "h", "40").c_str());
  g_cam.yaw = atof(nsc::opt(t, "yaw", "0").c_str()); g_cam.aimz = atof(nsc::opt(t, "aimz", "20").c_str());
  std::string wy = nsc::opt(t, "wyaw"); g_cam.wy = wy.empty() ? NAN : atof(wy.c_str());
  int clear = -1;
  if (isnan(g_cam.wy)) {
    V3 tp{}; double ty = ParseNum(CallO(tgt, "K2_GetActorRotation", ""), "Yaw=");
    if (!ActorPos(tgt, tp) || isnan(ty)) return "err cam cannot read the target";
    const double offs[] = { 0, 45, -45, 90, -90, 135, -135, 180 };
    for (int i = 0; i < 8 && clear < 0; i++) {
      double a = (ty + g_cam.yaw + offs[i]) / 57.29577951308232; bool ok = true;
      for (double d : { g_cam.dist, g_cam.dist + 60 })
        for (double tz : { tp.z, tp.z + g_cam.aimz + 40 }) {
          V3 s{ tp.x + cos(a) * d, tp.y + sin(a) * d, tp.z + g_cam.h };
          double full = sqrt((s.x - tp.x) * (s.x - tp.x) + (s.y - tp.y) * (s.y - tp.y) + (s.z - tz) * (s.z - tz));
          std::string r = Call("Default__KismetSystemLibrary", "LineTraceSingle", Fmt("pc %.1f,%.1f,%.1f %.1f,%.1f,%.1f TraceTypeQuery2 true [] None - true 1,0,0,1 0,1,0,1 0", s.x, s.y, s.z, tp.x, tp.y, tz).c_str());
          if (r.rfind("ok", 0) != 0) { ok = false; continue; }
          if (r.find("ReturnValue = true") != std::string::npos && ParseNum(r, "Distance =") < full - 60) ok = false;
        }
      if (ok) { clear = i; g_cam.wy = ty + g_cam.yaw + offs[i]; }
    }
    if (clear < 0) g_cam.wy = ty + g_cam.yaw;
  }
  V3 sp{}; ActorPos(tgt, sp);
  if (!CamEnsure(V3{sp.x, sp.y, sp.z + 100})) return "err camera spawn failed";
  if (!CamPlace()) { CamOff("target lost"); return "err cam cannot place on target"; }
  Call("pc", "SetViewTargetWithBlend", Fmt("0x%llx 0 VTBlend_Linear 0 false", (unsigned long long)g_cam.cam).c_str());
  uintptr_t vt = RetAddr(Call("pc", "GetViewTarget", ""));
  g_cam.on = true;
  Log("cam on tgt=0x%llx cam=0x%llx dist=%.0f h=%.0f yaw=%.0f view=%s", (unsigned long long)tgt, (unsigned long long)g_cam.cam, g_cam.dist, g_cam.h, g_cam.yaw, vt == g_cam.cam ? "camera" : "NOT camera");
  if (vt != g_cam.cam) return "err view target did not switch to the camera";
  return Fmt("cam on 0x%llx at=%.0f,%.0f,%.0f wyaw=%.1f sight=%s", (unsigned long long)g_cam.cam, g_cam.at.x, g_cam.at.y, g_cam.at.z, g_cam.wy, wy.empty() ? (clear < 0 ? "blocked" : "clear") : "given");
}
static void TickCam()
{
  if (!g_cam.on) return;
  if (!g_cam.detached) { if (!CamPlace()) CamOff("target lost"); return; }
  if (g_cam.orbit) PlaceOrbit();
  else if (g_cam.play) TickKeys();
}
static bool PlayOn() { return P.on; }
static std::string PlayRes() { return P.result; }

static void PollRequests()
{
  WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA((g_io + "\\req_*.txt").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  std::vector<std::string> names;
  do names.push_back(fd.cFileName); while (FindNextFileA(h, &fd));
  FindClose(h);
  for (auto& n : names) {
    std::string path = g_io + "\\" + n, body;
    FILE* fp = fopen(path.c_str(), "rb"); if (!fp) continue;
    char b[8192]; size_t k; while ((k = fread(b, 1, sizeof b, fp)) > 0) body.append(b, k); fclose(fp);
    DeleteFileA(path.c_str());
    std::string id = n.substr(4, n.size() - 8), out;
    size_t i = 0;
    while (i < body.size()) { size_t j = body.find('\n', i); if (j == std::string::npos) j = body.size(); std::string ln = body.substr(i, j - i); if (!ln.empty() && ln.back() == '\r') ln.pop_back(); if (!ln.empty()) out += Exec(ln) + "\n"; i = j + 1; }
    std::string tmp = g_io + "\\res_" + id + ".tmp", fin = g_io + "\\res_" + id + ".txt";
    fp = fopen(tmp.c_str(), "wb"); if (fp) { fwrite(out.data(), 1, out.size(), fp); fclose(fp); MoveFileExA(tmp.c_str(), fin.c_str(), MOVEFILE_REPLACE_EXISTING); }
  }
}

S2_EXPORT int S2PluginInit(const S2Api* a)
{
  api = a;
  char ps[16] = {0}; GetEnvironmentVariableA("ANOMALY_PORT", ps, sizeof ps); g_port = atoi(ps);
  g_on = S2Env("ANOMALY_NBOT", "1") != "0";
  if (!g_on) { api->log("nbot off (ANOMALY_NBOT=0) on port %d", g_port); return 1; }
  g_io = S2Path("bench\\io\\") + std::to_string(g_port);
  CreateDirectoryA(S2Path("bench").c_str(), nullptr); CreateDirectoryA(S2Path("bench\\io").c_str(), nullptr); CreateDirectoryA(g_io.c_str(), nullptr);
  { char bb[MAX_PATH] = {0}; GetEnvironmentVariableA("S2_BRIDGE", bb, sizeof bb); g_bridge = bb; if (!g_bridge.empty() && g_bridge.back() != '\\') g_bridge += '\\'; }
  api->log("nbot on port %d io %s bridge %s", g_port, g_io.c_str(), g_bridge.c_str()); Log("nbot loaded port %d bridge %s", g_port, g_bridge.c_str()); Nat();
  return 1;
}
S2_EXPORT void S2PluginShutdown() { if (g_on) CamOff("unload"); if (g_on && g.kind != G_NONE) Finish("aborted", "unload"); if (R.on) RecStop(); }
S2_EXPORT void S2PluginTick()
{
  if (!g_on) return;
  g_tick++;
  RecHotkey();
  if (R.on) { RecKeys(); RecMode(); RecSample(); }
  if (g_tick % 3 == 0) PollRequests();
  if (g_tick % 3 == 2) SceneTick();
  if (g_tick % 3 == 0) TickHpGuard();
  if (g_tick % 3 == 2) TickImmune();
  TickCam();
  if (g.kind == G_HOLD) { if (GetTickCount64() >= g.until) Finish("done", "held"); else HoldNat(0); return; }
  if (g_tick % 3 != 1) return;
  if (g_tick % 15 == 1) Crumb();
  PlayTick();
  if (g.kind == G_GOTO) TickGoto();
  else if (g.kind == G_NONE && g_set.on) TickSettle();
  else if (g.kind == G_FIRE && GetTickCount64() >= g.until) Finish("done", "fired");
  else if (g.kind == G_HOLDMOVE && GetTickCount64() >= g.until) Finish("done", "moved");
  else if (g.kind == G_USE) TickUse();
}
