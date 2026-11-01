#pragma once
#include <regex>
#include <set>

static std::string g_bridge;
static bool PlayOn();
static std::string PlayRes();
static std::string PlayJson();

static uint64_t WallMs()
{
  FILETIME ft; GetSystemTimeAsFileTime(&ft);
  ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
  return (u.QuadPart - 116444736000000000ULL) / 10000ULL;
}
static bool ReadFile2(const std::string& p, std::string& out)
{
  out.clear(); FILE* fp = fopen(p.c_str(), "rb"); if (!fp) return false;
  char b[65536]; size_t k; while ((k = fread(b, 1, sizeof b, fp)) > 0) out.append(b, k); fclose(fp); return true;
}
static bool WriteFile2(const std::string& p, const std::string& body)
{
  std::string tmp = p + ".tmp"; FILE* fp = fopen(tmp.c_str(), "wb"); if (!fp) return false;
  fwrite(body.data(), 1, body.size(), fp); fclose(fp);
  return MoveFileExA(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}
static bool Exists(const std::string& p) { return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

struct CensusJob { int st = 0; uint64_t t0 = 0; std::string err; nsc::Census c; uint64_t doneT = 0; };
static CensusJob g_cj;
static std::string CensusPath() { return g_bridge + "nbot_census.json"; }
static void CensusStart() { g_cj = CensusJob(); g_cj.st = 1; g_cj.t0 = GetTickCount64(); }
static void CensusPoll()
{
  if (g_cj.st == 0 || g_cj.st == 3 || g_cj.st == 4) return;
  uint64_t now = GetTickCount64();
  if (g_bridge.empty()) { g_cj.st = 4; g_cj.err = "no S2_BRIDGE"; return; }
  if (now - g_cj.t0 > 15000) { g_cj.st = 4; g_cj.err = g_cj.st == 1 ? "s2cmd.txt busy" : "census not written in 15 s (dev DLL with devcmd=1 needed)"; return; }
  std::string in = g_bridge + "s2cmd.txt";
  if (g_cj.st == 1) {
    if (Exists(in)) return;
    DeleteFileA(CensusPath().c_str());
    if (!WriteFile2(in, "census " + CensusPath() + "\n")) { g_cj.st = 4; g_cj.err = "cannot write s2cmd.txt"; return; }
    g_cj.st = 2; return;
  }
  if (g_cj.st == 2) {
    if (Exists(in) || !Exists(CensusPath())) return;
    std::string txt; if (!ReadFile2(CensusPath(), txt) || txt.empty()) return;
    g_cj.c = nsc::parse_census(txt);
    if (!g_cj.c.ok) { g_cj.st = 4; g_cj.err = g_cj.c.err; return; }
    g_cj.st = 3; g_cj.doneT = now;
  }
}

static std::vector<uintptr_t> FindAll(const std::string& q, int mx)
{
  std::vector<uintptr_t> r; std::string o = Cmd(Fmt("find %s %d", q.c_str(), mx));
  size_t i = 0;
  while (i < o.size()) {
    size_t e = o.find('\n', i); if (e == std::string::npos) e = o.size();
    std::string l = o.substr(i, e - i); i = e + 1;
    if (l.rfind("0x", 0) == 0 && l.find("Default__") == std::string::npos) r.push_back((uintptr_t)_strtoui64(l.c_str(), nullptr, 16));
  }
  return r;
}
static bool ActorPos(uintptr_t a, V3& v) { return ParseV(CallO(a, "K2_GetActorLocation", ""), v); }
static uintptr_t Nearest(const std::vector<uintptr_t>& c, V3 at, double maxd)
{
  uintptr_t best = 0; double bd = maxd;
  for (auto a : c) { V3 p; if (!ActorPos(a, p)) continue; double d = sqrt((p.x - at.x) * (p.x - at.x) + (p.y - at.y) * (p.y - at.y) + (p.z - at.z) * (p.z - at.z)); if (d < bd) { bd = d; best = a; } }
  return best;
}
static std::string RetVal(const std::string& r);
static uintptr_t NearestVisible(const std::vector<uintptr_t>& c, V3 at, double maxd)
{
  std::vector<uintptr_t> addrs; std::vector<nsc::PickCand> pc;
  for (auto a : c) {
    V3 p; if (!ActorPos(a, p)) continue;
    double d = sqrt((p.x - at.x) * (p.x - at.x) + (p.y - at.y) * (p.y - at.y) + (p.z - at.z) * (p.z - at.z));
    if (d >= maxd) continue;
    char hb[32] = {0}; api->get(a, "bHidden", hb, sizeof hb);
    nsc::PickCand k; k.dist = d; k.hidden = strstr(hb, "true") != nullptr || strcmp(hb, "1") == 0;
    k.rendered = RetVal(CallO(a, "WasRecentlyRendered", "0.2")) != "false";
    addrs.push_back(a); pc.push_back(k);
  }
  int i = nsc::pick_visible(pc); return i < 0 ? 0 : addrs[i];
}
static uintptr_t ClassObj(const char* cls)
{
  static std::map<std::string, uintptr_t> cache;
  auto it = cache.find(cls); if (it != cache.end() && Valid(it->second)) return it->second;
  auto v = FindAll(std::string("class:") + cls, 20); uintptr_t o = v.empty() ? 0 : v[0]; cache[cls] = o; return o;
}
static std::string RetVal(const std::string& r)
{
  size_t q = r.find("ReturnValue = "); if (q == std::string::npos) return ""; std::string v = r.substr(q + 14);
  size_t e = v.find('\n'); if (e != std::string::npos) v = v.substr(0, e); while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back(); return v;
}
static bool OutV(const std::string& r, const char* name, V3& v)
{
  size_t q = r.find(std::string("out ") + name + " = "); if (q == std::string::npos) return false;
  std::string s = r.substr(q); size_t e = s.find('\n'); if (e != std::string::npos) s = s.substr(0, e);
  return ParseV(s, v);
}
static bool Out2(const std::string& r, const char* name, double& x, double& y)
{
  size_t q = r.find(std::string("out ") + name + " = "); if (q == std::string::npos) return false;
  std::string s = r.substr(q); size_t e = s.find('\n'); if (e != std::string::npos) s = s.substr(0, e);
  size_t a = s.find("X="), b = s.find("Y="); if (a == std::string::npos || b == std::string::npos) return false;
  x = atof(s.c_str() + a + 2); y = atof(s.c_str() + b + 2); return true;
}
static double OutN(const std::string& r, const char* name)
{
  size_t q = r.find(std::string("out ") + name + " = "); if (q == std::string::npos) return NAN; return atof(r.c_str() + q + strlen(name) + 7);
}

struct SVar { std::string key, cls; uintptr_t addr = 0; V3 pos{}; };
struct SRes { int n = 0, line = 0; std::string text, op, result, detail, data; uint64_t ms = 0; };
struct Scene {
  bool on = false; std::string name, path, state = "idle"; std::vector<nsc::Step> steps; size_t i = 0; int phase = 0;
  uint64_t t0 = 0, stepT0 = 0, wake = 0; std::vector<SRes> res; std::map<std::string, SVar> vars;
  std::set<std::string> before; std::set<uintptr_t> pawns; uintptr_t newA = 0; std::string frz; V3 at{}; double yaw = 0; long long logOff = -1, prevLogOff = -1, curLogOff = -1; uint64_t lastCensus = 0; std::string err;
};
static Scene S;

static std::string ResultPath() { return g_io + "\\scene_result.json"; }
static std::string SceneJson()
{
  std::string o = Fmt("{\"scene\":\"%s\",\"path\":\"%s\",\"state\":\"%s\",\"port\":%d,\"steps\":%zu,\"done\":%zu,\"ms\":%llu,\"err\":\"%s\",\"results\":[",
    nsc::jesc(S.name).c_str(), nsc::jesc(S.path).c_str(), S.state.c_str(), g_port, S.steps.size(), S.res.size(), (unsigned long long)(S.t0 ? GetTickCount64() - S.t0 : 0), nsc::jesc(S.err).c_str());
  for (size_t k = 0; k < S.res.size(); k++) {
    auto& r = S.res[k];
    o += Fmt("%s\n {\"n\":%d,\"line\":%d,\"op\":\"%s\",\"text\":\"%s\",\"result\":\"%s\",\"ms\":%llu,\"detail\":\"%s\",\"data\":%s}", k ? "," : "", r.n, r.line, r.op.c_str(), nsc::jesc(r.text).c_str(), r.result.c_str(), (unsigned long long)r.ms, nsc::jesc(r.detail).c_str(), r.data.empty() ? "null" : r.data.c_str());
  }
  return o + "\n]}\n";
}
static void SceneSave() { WriteFile2(ResultPath(), SceneJson()); }

static std::string ExeDir()
{
  char p[MAX_PATH] = {0}; GetModuleFileNameA(nullptr, p, MAX_PATH); std::string s = p; size_t k = s.find_last_of('\\'); return k == std::string::npos ? s : s.substr(0, k);
}
static std::string LogFile() { return ExeDir() + "\\ue4ss\\UE4SS.log"; }
static long long FileSize(const std::string& p) { WIN32_FILE_ATTRIBUTE_DATA d; if (!GetFileAttributesExA(p.c_str(), GetFileExInfoStandard, &d)) return -1; return ((long long)d.nFileSizeHigh << 32) | d.nFileSizeLow; }

static int CloseUi(std::string& det)
{
  int n = 0;
  const char* cls[] = { "InventoryNew" };
  for (auto c : cls) for (auto a : FindAll(std::string("isa:") + c, 20)) {
    std::string iv = RetVal(CallO(a, "IsInViewport", ""));
    if (iv != "true") continue;
    std::string r = CallO(a, "UIClose", ""); n++; det += Fmt("%s 0x%llx UIClose %s; ", c, (unsigned long long)a, r.substr(0, 20).c_str());
  }
  return n;
}

static bool Resolve(const std::string& spec, const nsc::Census* c, SVar& out, std::string& why)
{
  if (spec == "me" || spec == "player:local") { out.key = "player:local"; out.addr = Find("pawn"); if (!out.addr || !Pos(out.pos)) { why = "no pawn"; return false; } return true; }
  if (spec[0] == '$') {
    auto it = S.vars.find(spec.substr(1)); if (it == S.vars.end()) { why = "unset var " + spec; return false; }
    out = it->second;
    if (out.addr && Valid(out.addr) && ActorPos(out.addr, out.pos)) return true;
    if (!c) { why = "var actor gone and no census"; return false; }
    return Resolve(out.key, c, out, why);
  }
  if (!c) { why = "no census"; return false; }
  if (spec == "peer") {
    if (c->peers.empty()) { why = "no peer in census"; return false; }
    const nsc::CRow* best = nullptr; double bd = 1e18;
    for (auto& x : c->rows) if (x.fam == "body" && x.key.rfind("puppet:", 0) == 0) { double dx = x.num("x", 0) - c->peers[0].x, dy = x.num("y", 0) - c->peers[0].y, d = dx * dx + dy * dy; if (d < bd) { bd = d; best = &x; } }
    if (!best) { why = "no puppet body row for the peer"; return false; }
    return Resolve(best->key, c, out, why);
  }
  const nsc::CRow* r = c->find("npc", spec);
  if (!r) r = c->find("*", spec);
  if (!r) { why = "key " + spec + " not in census"; return false; }
  out.key = spec; out.cls = r->str("cls"); out.pos = V3{ r->num("x", 0), r->num("y", 0), r->num("z", 0) };
  if (spec == "player:local") { out.addr = Find("pawn"); return out.addr != 0; }
  std::vector<uintptr_t> cand = !out.cls.empty() ? FindAll("class:" + out.cls, 200) : FindAll("isa:Pawn", 300);
  out.addr = NearestVisible(cand, out.pos, out.cls.empty() ? 120 : 250);
  if (!out.addr) { why = Fmt("no actor of %s within range of %.0f,%.0f,%.0f (%zu candidates)", out.cls.empty() ? "Pawn" : out.cls.c_str(), out.pos.x, out.pos.y, out.pos.z, cand.size()); return false; }
  ActorPos(out.addr, out.pos);
  return true;
}

static bool Bounds(uintptr_t a, V3& org, V3& ext)
{
  V3 at{}; if (!ActorPos(a, at)) return false;
  char mo[512] = {0}; api->get(a, "Mesh", mo, sizeof mo); const char* mx = strstr(mo, "0x");
  uintptr_t mesh = mx ? (uintptr_t)_strtoui64(mx, nullptr, 16) : 0;
  V3 pel{}; if (mesh && RetVal(CallO(mesh, "DoesSocketExist", "pelvis")) == "true" && ParseV(CallO(mesh, "GetSocketLocation", "pelvis"), pel) && D2(pel, at) < 300 && at.z - pel.z > 50) { org = pel; ext = V3{ 90, 90, 35 }; return true; }
  const char* modes[] = { "true false", "false false" };
  for (auto m : modes) {
    V3 o{}, e{}; std::string bs = CallO(a, "GetActorBounds", m);
    if (!OutV(bs, "Origin", o) || !OutV(bs, "BoxExtent", e)) continue;
    if (e.x < 5 || e.z < 5 || e.x > 400 || e.z > 400 || D2(o, at) > 300 || fabs(o.z - at.z) > 300) continue;
    org = o; ext = e; return true;
  }
  org = V3{ at.x, at.y, at.z }; ext = V3{ 40, 40, 90 }; return true;
}

struct Proj { bool ok = false; double vw = 0, vh = 0; nsc::Box b; V3 cam{}, org{}, ext{}; double dist = 0; std::string rendered = "?"; int front = 0; };
static Proj Project(uintptr_t a)
{
  Proj p; uintptr_t pc = Find("pc"); if (!pc || !a) return p;
  std::string vs = CallO(pc, "GetViewportSize", ""); p.vw = OutN(vs, "SizeX"); p.vh = OutN(vs, "SizeY");
  if (!Bounds(a, p.org, p.ext)) return p;
  uintptr_t pcm = ObjAddr("pc", "PlayerCameraManager");
  if (pcm) ParseV(CallO(pcm, "GetCameraLocation", ""), p.cam);
  p.dist = sqrt((p.org.x - p.cam.x) * (p.org.x - p.cam.x) + (p.org.y - p.cam.y) * (p.org.y - p.cam.y) + (p.org.z - p.cam.z) * (p.org.z - p.cam.z));
  double cs[8][3]; nsc::bounds_corners(p.org.x, p.org.y, p.org.z, p.ext.x, p.ext.y, p.ext.z, cs);
  std::vector<std::pair<bool, std::pair<double, double>>> pts;
  for (int k = 0; k < 8; k++) {
    std::string r = CallO(pc, "ProjectWorldLocationToScreen", Fmt("%.1f,%.1f,%.1f false", cs[k][0], cs[k][1], cs[k][2]).c_str());
    double x = 0, y = 0; bool f = RetVal(r) == "true" && Out2(r, "ScreenLocation", x, y);
    pts.push_back({ f, { x, y } });
  }
  p.b = nsc::screen_box(pts, isnan(p.vw) ? 0 : p.vw, isnan(p.vh) ? 0 : p.vh); p.front = p.b.inFront;
  p.rendered = RetVal(CallO(a, "WasRecentlyRendered", "0.2"));
  p.ok = true; return p;
}
static std::string ProjJson(const SVar& v, const Proj& p, uint64_t wall)
{
  return Fmt("{\"key\":\"%s\",\"cls\":\"%s\",\"addr\":\"0x%llx\",\"wall_ms\":%llu,\"viewport\":[%.0f,%.0f],\"cam\":[%.1f,%.1f,%.1f],\"origin\":[%.1f,%.1f,%.1f],\"extent\":[%.1f,%.1f,%.1f],\"dist\":%.1f,\"rendered\":\"%s\",\"onscreen\":%s,\"infront\":%d,\"box\":[%.1f,%.1f,%.1f,%.1f],\"vis\":%.3f}",
    nsc::jesc(v.key).c_str(), nsc::jesc(v.cls).c_str(), (unsigned long long)v.addr, (unsigned long long)wall, isnan(p.vw) ? 0.0 : p.vw, isnan(p.vh) ? 0.0 : p.vh, p.cam.x, p.cam.y, p.cam.z, p.org.x, p.org.y, p.org.z, p.ext.x, p.ext.y, p.ext.z, p.dist, p.rendered.c_str(), p.b.any && p.b.vis > 0 ? "true" : "false", p.front, p.b.x0, p.b.y0, p.b.x1, p.b.y1, p.b.vis);
}

static bool PeerPos(const nsc::Census& c, V3& v)
{
  if (c.peers.empty()) return false;
  double z = NAN, bd = 1e18; V3 me{}; Pos(me);
  for (auto& r : c.rows) if (r.fam == "body" && r.key.rfind("puppet:", 0) == 0) { double dx = r.num("x", 0) - c.peers[0].x, dy = r.num("y", 0) - c.peers[0].y, d = dx * dx + dy * dy; if (d < bd) { bd = d; z = r.num("z", NAN); } }
  v = V3{ c.peers[0].x, c.peers[0].y, isnan(z) || bd > 40000 ? me.z : z }; return true;
}

static int StepNeedsCensus(const nsc::Step& s)
{
  const auto& o = s.op; const auto& t = s.t;
  if (o == "require" || o == "spawn" || o == "expect") return 1;
  if (o == "wait" && t.size() >= 2 && t[1] == "census") return 1;
  if (o == "tp" && t.size() >= 2 && (t[1].rfind("peer+", 0) == 0 || (t[1][0] == '$'))) return 1;
  if ((o == "frame" || o == "box" || o == "kill") && t.size() >= 2 && t[1][0] != '$' && t[1] != "me") return 1;
  return 0;
}

static std::vector<std::string> ExpandVars(const std::vector<std::string>& in)
{
  std::vector<std::string> o = in;
  for (size_t k = 1; k < o.size(); k++) if (o[k].size() > 1 && o[k][0] == '$') { auto it = S.vars.find(o[k].substr(1)); if (it != S.vars.end()) o[k] = it->second.key; }
  return o;
}
static int RunStep(const nsc::Step& s, SRes& r)
{
  const std::vector<std::string> tx = (s.op == "wait" || s.op == "expect") ? ExpandVars(s.t) : s.t;
  const auto& t = tx; const std::string& o = s.op; uint64_t now = GetTickCount64(); double el = (now - S.stepT0) / 1000.0;
  if (S.phase == 0 && StepNeedsCensus(s)) { CensusStart(); S.phase = 1; return 0; }
  if (S.phase == 1) { CensusPoll(); if (g_cj.st == 4) { r.detail = "census: " + g_cj.err; return -1; } if (g_cj.st != 3) return 0; S.phase = 2; }
  const nsc::Census* c = g_cj.st == 3 && g_cj.doneT >= S.stepT0 ? &g_cj.c : nullptr;
  if (o == "note") { r.detail = nsc::opt(t, "text", s.text.size() > 5 ? s.text.substr(5) : ""); return 1; }
  if (o == "closeui" || o == "hudclean") { std::string d; int n = CloseUi(d); r.detail = Fmt("closed %d %s", n, d.c_str()); return 1; }
  if (o == "require") {
    double rad = atof(t[2].c_str()); std::vector<std::string> allow; std::string al = nsc::opt(t, "allow");
    size_t i = 0; while (i <= al.size() && !al.empty()) { size_t j = al.find(',', i); if (j == std::string::npos) j = al.size(); allow.push_back(al.substr(i, j - i)); i = j + 1; }
    auto h = nsc::hostiles(*c, rad, allow);
    std::string d; for (auto* x : h) d += Fmt("%s %s %.0fcm; ", x->key.c_str(), x->str("cls").c_str(), x->num("dist", -1));
    r.detail = Fmt("%zu live non-player npc within %.0f cm %s", h.size(), rad, d.c_str());
    r.data = Fmt("{\"count\":%zu,\"radius\":%.0f}", h.size(), rad);
    return h.empty() ? 1 : -1;
  }
  if (o == "spawn") {
    if (S.phase == 2) {
      S.before.clear(); for (auto& x : c->rows) if (x.fam == "npc") S.before.insert(x.key);
      S.pawns.clear(); for (auto a : FindAll("isa:Pawn", 3000)) S.pawns.insert(a);
      V3 p{}, rt{}; if (!Pos(p) || !Rot(rt)) { r.detail = "no pawn"; return -1; }
      double m = 3; nsc::parse_rel(nsc::opt(t, "at", "player+3"), "player", m);
      double yr = rt.y / 57.29578; S.at = V3{ p.x + cos(yr) * m * 100, p.y + sin(yr) * m * 100, p.z + 50 };
      std::string ys = nsc::opt(t, "yaw"); S.yaw = ys.empty() ? rt.y + 180 : atof(ys.c_str());
      uintptr_t rk = ClassObj("CustomConsoleManagerRK"); if (!rk) { r.detail = "no CustomConsoleManagerRK"; return -1; }
      std::string cr = CallO(rk, "XSpawnObjBySID", Fmt("%s 1 false %.1f %.1f %.1f 0 %.1f 0", nsc::opt(t, "sid").c_str(), S.at.x, S.at.y, S.at.z, S.yaw).c_str());
      r.detail = "XSpawnObjBySID " + cr.substr(0, 60);
      if (cr.rfind("ok", 0) != 0) return -1;
      S.newA = 0; S.frz.clear(); S.phase = 10; return 0;
    }
    if (S.phase == 10) {
      for (auto a : FindAll("isa:Pawn", 3000)) if (!S.pawns.count(a)) { V3 q; if (ActorPos(a, q) && D2(q, S.at) < 1000) { S.newA = a; break; } }
      if (!S.newA) { if (el > 4) { S.phase = 3; S.wake = now; } return 0; }
      if (nsc::opt(t, "freeze") == "1") { char so[256] = {0}; api->set(S.newA, "CustomTimeDilation", "0.001", so, sizeof so); char go[64] = {0}; api->get(S.newA, "CustomTimeDilation", go, sizeof go); S.frz = Fmt(" freeze CustomTimeDilation=%s after %.2f s", go, el); }
      ActorPos(S.newA, S.at); S.phase = 3; S.wake = now + 300; return 0;
    }
    if (S.phase == 3) { if (now < S.wake) return 0; CensusStart(); S.phase = 4; return 0; }
    if (S.phase == 4) {
      CensusPoll(); if (g_cj.st == 4) { r.detail += "; census: " + g_cj.err; return -1; } if (g_cj.st != 3) return 0;
      const nsc::CRow* best = nullptr; double bd = 800;
      for (auto& x : g_cj.c.rows) if (x.fam == "npc" && !S.before.count(x.key) && x.str("player") != "true") { double dx = x.num("x", 0) - S.at.x, dy = x.num("y", 0) - S.at.y, d = sqrt(dx * dx + dy * dy); if (d < bd) { bd = d; best = &x; } }
      if (!best) { if (el > s.timeout) { r.detail += "; no new npc row near the spawn point"; return -1; } S.phase = 3; S.wake = now + 1000; return 0; }
      SVar v; std::string why;
      if (!Resolve(best->key, &g_cj.c, v, why)) { r.detail += "; " + why; return -1; }
      if (S.newA && Valid(S.newA)) { v.addr = S.newA; ActorPos(v.addr, v.pos); }
      std::string fr = S.frz;
      if (fr.empty() && nsc::opt(t, "freeze") == "1") { char so[256] = {0}; api->set(v.addr, "CustomTimeDilation", "0.001", so, sizeof so); char go[64] = {0}; api->get(v.addr, "CustomTimeDilation", go, sizeof go); fr = Fmt(" late freeze CustomTimeDilation=%s", go); }
      std::string as = nsc::opt(t, "as", "spawn"); S.vars[as] = v;
      r.detail += Fmt("; $%s = %s %s 0x%llx at %.0f,%.0f,%.0f (%.0f cm from the spawn point)%s", as.c_str(), v.key.c_str(), v.cls.c_str(), (unsigned long long)v.addr, v.pos.x, v.pos.y, v.pos.z, bd, fr.c_str());
      r.data = Fmt("{\"var\":\"%s\",\"key\":\"%s\",\"cls\":\"%s\",\"addr\":\"0x%llx\",\"pos\":[%.1f,%.1f,%.1f]}", as.c_str(), nsc::jesc(v.key).c_str(), nsc::jesc(v.cls).c_str(), (unsigned long long)v.addr, v.pos.x, v.pos.y, v.pos.z);
      return 1;
    }
  }
  if (o == "kill") {
    SVar v; std::string why; if (!Resolve(t[1], c, v, why)) { r.detail = why; return -1; }
    char so[256] = {0}; api->set(v.addr, "CustomTimeDilation", "1", so, sizeof so);
    std::string k = CallO(v.addr, "ForceSetHP", "0"); r.detail = Fmt("%s 0x%llx CustomTimeDilation=1, ForceSetHP 0 -> %s", v.key.c_str(), (unsigned long long)v.addr, k.substr(0, 40).c_str());
    return k.rfind("ok", 0) == 0 ? 1 : -1;
  }
  if (o == "faction") {
    bool ok; int v = nsc::relation_value(t[1], ok); if (!ok) { r.detail = "bad relation " + t[1]; return -1; }
    uintptr_t m = ClassObj("CustomConsoleManagerAI2"); if (!m) { r.detail = "no CustomConsoleManagerAI2"; return -1; }
    std::string k = CallO(m, "XSetRelationBetweenFactions", Fmt("%s %s %d", t[2].c_str(), t[3].c_str(), v).c_str());
    r.detail = Fmt("XSetRelationBetweenFactions %s %s %d -> %s", t[2].c_str(), t[3].c_str(), v, k.substr(0, 40).c_str()); return k.rfind("ok", 0) == 0 ? 1 : -1;
  }
  if (o == "give") {
    uintptr_t m = ClassObj("CustomConsoleManagerRK"); if (!m) { r.detail = "no CustomConsoleManagerRK"; return -1; }
    std::string k = CallO(m, "XCreateItemInInventoryByID", Fmt("%s 0 %d 1", t[1].c_str(), t.size() > 2 ? atoi(t[2].c_str()) : 1).c_str());
    r.detail = "XCreateItemInInventoryByID " + t[1] + " -> " + k.substr(0, 40); return k.rfind("ok", 0) == 0 ? 1 : -1;
  }
  if (o == "hp") {
    std::string k = Call("pawn", "ForceSetHP", t[1].c_str()); double h = ParseNum(Call("pawn", "GetHP", ""), "ReturnValue");
    r.detail = Fmt("ForceSetHP %s -> %s; GetHP=%.1f", t[1].c_str(), k.substr(0, 20).c_str(), h); return k.rfind("ok", 0) == 0 ? 1 : -1;
  }
  if (o == "god") {
    uintptr_t m = ClassObj("CustomConsoleManagerRK"); if (!m) { r.detail = "no CustomConsoleManagerRK"; return -1; }
    std::string k = CallO(m, "XSetGodMode", t[1] == "1" ? "true" : "false"); r.detail = "XSetGodMode " + t[1] + " -> " + k.substr(0, 40); return k.rfind("ok", 0) == 0 ? 1 : -1;
  }
  if (o == "time") {
    int h, mi; nsc::parse_hhmm(t[1], h, mi); uintptr_t m = ClassObj("CustomConsoleManagerAA"); if (!m) { r.detail = "no CustomConsoleManagerAA"; return -1; }
    std::string k = CallO(m, "XSetGameTime", Fmt("%d %d 0", h, mi).c_str()); r.detail = Fmt("XSetGameTime %d %d 0 -> %s", h, mi, k.substr(0, 40).c_str()); return k.rfind("ok", 0) == 0 ? 1 : -1;
  }
  if (o == "wait" && t.size() >= 2 && t[1] == "census") {
    nsc::Query q = nsc::parse_query(t, 2); auto hits = nsc::run_query(*c, q);
    if ((int)hits.size() >= q.minCount) { r.detail = Fmt("%zu rows match after %.1f s", hits.size(), el); return 1; }
    if (el > s.timeout) { r.detail = Fmt("timeout %.0f s: %zu rows match, need %d", s.timeout, hits.size(), q.minCount); return -1; }
    if (S.phase == 2) { S.phase = 5; S.wake = now + 1000; return 0; }
    return 0;
  }
  if (o == "wait" && t.size() >= 3 && t[1] == "log") {
    std::string lp = LogFile();
    if (S.logOff < 0) { S.logOff = S.prevLogOff >= 0 ? S.prevLogOff : FileSize(lp); if (S.logOff < 0) { r.detail = "no log " + lp; return -1; } return 0; }
    long long sz = FileSize(lp);
    if (sz > S.logOff) {
      FILE* fp = fopen(lp.c_str(), "rb");
      if (fp) {
        std::string txt; _fseeki64(fp, S.logOff, SEEK_SET); txt.resize((size_t)(sz - S.logOff)); size_t got = fread(&txt[0], 1, txt.size(), fp); fclose(fp); txt.resize(got);
        size_t lastNl = txt.find_last_of('\n');
        if (lastNl != std::string::npos) {
          std::string body = txt.substr(0, lastNl + 1); S.logOff += (long long)body.size();
          try {
            std::regex re(t[2]); size_t i = 0;
            while (i < body.size()) { size_t e = body.find('\n', i); std::string l = body.substr(i, e - i); i = e + 1; if (std::regex_search(l, re)) { while (!l.empty() && (l.back() == '\r')) l.pop_back(); r.detail = l.substr(0, 300); return 1; } }
          } catch (...) { r.detail = "bad regex " + t[2]; return -1; }
        }
      }
    }
    if (el > s.timeout) { r.detail = Fmt("timeout %.0f s waiting for /%s/", s.timeout, t[2].c_str()); return -1; }
    return 0;
  }
  if (o == "wait") { if (el >= atof(t[1].c_str())) { r.detail = Fmt("waited %.1f s", el); return 1; } return 0; }
  if (o == "expect") {
    nsc::Query q = nsc::parse_query(t, 2); auto hits = nsc::run_query(*c, q);
    std::string d; for (size_t k = 0; k < hits.size() && k < 5; k++) d += hits[k]->key + " ";
    r.detail = Fmt("%zu rows match (need %d) %s", hits.size(), q.minCount, d.c_str()); return (int)hits.size() >= q.minCount ? 1 : -1;
  }
  if (o == "tp") {
    if (S.phase <= 2) {
      V3 dst{}; double v; V3 p{}, rt{}; Pos(p); Rot(rt);
      if (sscanf(t[1].c_str(), "%lf,%lf,%lf", &dst.x, &dst.y, &dst.z) == 3) {}
      else if (nsc::parse_rel(t[1], "player", v)) { double yr = rt.y / 57.29578; dst = V3{ p.x + cos(yr) * v * 100, p.y + sin(yr) * v * 100, p.z }; }
      else if (nsc::parse_rel(t[1], "peer", v)) { V3 pp; if (!PeerPos(*c, pp)) { r.detail = "no peer in census"; return -1; } dst = V3{ pp.x + v, pp.y, pp.z }; }
      else { std::string name = t[1]; double off = 150; size_t pl = name.find('+'); if (pl != std::string::npos) { off = atof(name.c_str() + pl + 1); name = name.substr(0, pl); } SVar sv; std::string why; if (!Resolve(name, c, sv, why)) { r.detail = why; return -1; } dst = V3{ sv.pos.x + off, sv.pos.y, sv.pos.z }; }
      if (!SafeTeleport(dst, "scene")) { r.detail = Fmt("safe teleport refused at %.0f,%.0f,%.0f (no walkable floor)", dst.x, dst.y, dst.z); return -1; }
      S.at = dst; S.phase = 6; return 0;
    }
    if (g_set.on) { TickSettle(); return 0; }
    V3 p{}; Pos(p); bool og = OnGround();
    r.detail = Fmt("teleported near %.0f,%.0f,%.0f, now %.0f,%.0f,%.0f grounded=%d", S.at.x, S.at.y, S.at.z, p.x, p.y, p.z, og ? 1 : 0);
    r.data = Fmt("{\"pos\":[%.1f,%.1f,%.1f],\"grounded\":%s}", p.x, p.y, p.z, og ? "true" : "false");
    return og && D2(p, S.at) < 300 ? 1 : -1;
  }
  if (o == "frame") {
    SVar v; std::string why;
    if (S.phase <= 2) {
      if (!Resolve(t[1], c, v, why)) { r.detail = why; return -1; }
      S.vars["_frame"] = v;
      std::string fov = nsc::opt(t, "fov"); if (!fov.empty()) Cmd("con fov " + fov);
      if (nsc::opt(t, "move") == "1") {
        V3 p{}; Pos(p); double dist = atof(nsc::opt(t, "dist", "4").c_str()) * 100, ox, oy; nsc::stand_point(v.pos.x, v.pos.y, p.x, p.y, dist, ox, oy);
        if (!SafeTeleport(V3{ ox, oy, v.pos.z }, "frame")) { r.detail = "stand point has no walkable floor"; return -1; }
        S.phase = 7; return 0;
      }
      S.phase = 8;
    }
    if (S.phase == 7) { if (g_set.on) { TickSettle(); return 0; } S.phase = 8; }
    v = S.vars["_frame"];
    if (S.phase == 8) {
      V3 org{}, ext{}; if (!Bounds(v.addr, org, ext)) { r.detail = "target gone"; return -1; }
      uintptr_t pcm = ObjAddr("pc", "PlayerCameraManager"); V3 cam{}; if (!pcm || !ParseV(CallO(pcm, "GetCameraLocation", ""), cam)) { V3 p{}; Pos(p); cam = V3{ p.x, p.y, p.z + 60 }; }
      nsc::Rot lr = nsc::look_rot(cam.x, cam.y, cam.z, org.x, org.y, org.z); SetRot(lr.pitch, lr.yaw);
      S.at = V3{ lr.pitch, lr.yaw, 0 }; S.phase = 9; S.wake = now + 400; return 0;
    }
    if (now < S.wake) return 0;
    V3 rt{}; Rot(rt); double dy = fabs(rt.y - S.at.y); while (dy > 360) dy -= 360; if (dy > 180) dy = 360 - dy; double dp = fabs(rt.x - S.at.x); if (dp > 180) dp = 360 - dp;
    Proj p = Project(v.addr);
    r.data = ProjJson(v, p, WallMs());
    r.detail = Fmt("%s 0x%llx want pitch=%.1f yaw=%.1f got pitch=%.1f yaw=%.1f dist=%.0f box=%.0f,%.0f-%.0f,%.0f vis=%.2f rendered=%s", v.key.c_str(), (unsigned long long)v.addr, S.at.x, S.at.y, rt.x, rt.y, p.dist, p.b.x0, p.b.y0, p.b.x1, p.b.y1, p.b.vis, p.rendered.c_str());
    return dy < 2 && dp < 2 && p.ok && p.b.vis >= 0.3 ? 1 : -1;
  }
  if (o == "box") {
    SVar v; std::string why;
    std::string ats = nsc::opt(t, "at"); uint64_t at = ats.empty() ? 0 : _strtoui64(ats.c_str(), nullptr, 10);
    if (at && WallMs() + 2 < at) { if (el > s.timeout + 60) { r.detail = "at= never reached"; return -1; } return 0; }
    if (!Resolve(t[1], c, v, why)) { r.detail = why; return -1; }
    uint64_t w = WallMs(); Proj p = Project(v.addr);
    r.data = ProjJson(v, p, w);
    std::string out = nsc::opt(t, "out"); if (!out.empty()) WriteFile2(out, r.data + "\n");
    r.detail = Fmt("%s wall=%llu late=%lld ms dist=%.0f box=%.0f,%.0f-%.0f,%.0f vis=%.2f rendered=%s", v.key.c_str(), (unsigned long long)w, at ? (long long)(w - at) : 0LL, p.dist, p.b.x0, p.b.y0, p.b.x1, p.b.y1, p.b.vis, p.rendered.c_str());
    return p.ok ? 1 : -1;
  }
  if (o == "key" || o == "shot") { r.detail = o + " runs on the harness side, not in the plugin"; return -1; }
  if (o == "bot" && t.size() >= 2 && t[1] == "play") {
    if (S.phase <= 2) { std::string k = Exec(Rest(s.text, 1)); r.detail = k.substr(0, 300); if (k.rfind("err", 0) == 0) return -1; S.phase = 11; return 0; }
    if (PlayOn()) return 0;
    r.detail = PlayJson(); r.data = PlayJson(); return PlayRes() == "done" ? 1 : -1;
  }
  if (o == "bot") { std::string k = Exec(Rest(s.text, 1)); r.detail = k.substr(0, 300); return k.rfind("err", 0) == 0 ? -1 : 1; }
  r.detail = "unhandled"; return -1;
}

static int RunStepSeh(const nsc::Step& s, SRes& r, int* code)
{
  __try { return RunStep(s, r); } __except (EXCEPTION_EXECUTE_HANDLER) { *code = (int)GetExceptionCode(); return -2; }
}
static void SceneTick()
{
  if (!S.on) return;
  uint64_t now = GetTickCount64();
  if (S.i >= S.steps.size()) { S.on = false; S.state = "done"; for (auto& r : S.res) if (r.result != "PASS") S.state = "failed"; SceneSave(); Log("scene %s %s", S.name.c_str(), S.state.c_str()); return; }
  const nsc::Step& s = S.steps[S.i];
  if (S.phase == 5) { if (now < S.wake) return; CensusStart(); S.phase = 1; }
  SRes r; r.n = (int)S.i + 1; r.line = s.line; r.text = s.text; r.op = s.op;
  if (!S.res.empty() && S.res.back().n == r.n) { r = S.res.back(); S.res.pop_back(); }
  int code = 0, rc = RunStepSeh(s, r, &code);
  if (rc == -2) { rc = -1; r.detail = Fmt("exception 0x%x", code); }
  if (rc == 0 && (now - S.stepT0) / 1000.0 > s.timeout + 20) { rc = -1; r.detail += " (step hard timeout)"; }
  if (rc == 0) { S.res.push_back(r); S.res.back().result = "RUN"; return; }
  r.result = rc > 0 ? "PASS" : "FAIL"; r.ms = now - S.stepT0;
  S.res.push_back(r);
  Log("scene %s step %d line %d %s %s | %s", S.name.c_str(), r.n, r.line, r.result.c_str(), s.text.c_str(), r.detail.c_str());
  S.i++; S.phase = 0; S.stepT0 = now; S.logOff = -1; S.prevLogOff = S.curLogOff; S.curLogOff = FileSize(LogFile());
  if (rc < 0) { S.on = false; S.state = "failed"; S.err = Fmt("step %d failed", r.n); Log("scene %s failed at step %d", S.name.c_str(), r.n); }
  SceneSave();
}

static std::string SceneLoad(const std::string& name, const std::string& text, const std::string& path, long long logoff = -1)
{
  if (S.on) return "err scene " + S.name + " is running";
  std::vector<std::string> errs; auto st = nsc::parse_scene(text, errs);
  if (!errs.empty()) { std::string o = "err parse:"; for (auto& e : errs) o += " " + e + ";"; return o; }
  if (st.empty()) return "err empty scene";
  auto keep = S.vars; S = Scene(); S.vars = keep; S.on = true; S.name = name; S.path = path; S.steps = st; S.state = "running"; S.t0 = S.stepT0 = GetTickCount64();
  S.curLogOff = FileSize(LogFile()); S.prevLogOff = logoff >= 0 && logoff <= S.curLogOff ? logoff : S.curLogOff;
  DeleteFileA(ResultPath().c_str()); SceneSave();
  Log("scene %s start steps=%zu path=%s", name.c_str(), st.size(), path.c_str());
  return Fmt("scene %s running steps=%zu result=%s", name.c_str(), st.size(), ResultPath().c_str());
}

static std::string SceneCmd(const std::string& line)
{
  auto t = Split(line); if (t.size() < 2) return "err scene run <path> | line <step> | status | stop";
  if (t[1] == "status") return SceneJson();
  if (t[1] == "stop") { if (!S.on) return "idle"; S.on = false; S.state = "stopped"; SceneSave(); return "stopped"; }
  if (t[1] == "vars") { std::string o = "{"; bool f = true; for (auto& kv : S.vars) { o += Fmt("%s\"%s\":{\"key\":\"%s\",\"cls\":\"%s\",\"addr\":\"0x%llx\"}", f ? "" : ",", kv.first.c_str(), nsc::jesc(kv.second.key).c_str(), nsc::jesc(kv.second.cls).c_str(), (unsigned long long)kv.second.addr); f = false; } return o + "}"; }
  if (t[1] == "run" && t.size() >= 3) {
    std::string p = Rest(line, 2), txt; long long lo = -1;
    size_t lq = p.rfind(" logoff="); if (lq != std::string::npos) { lo = _strtoi64(p.c_str() + lq + 8, nullptr, 10); p = p.substr(0, lq); }
    if (!ReadFile2(p, txt)) return "err cannot read " + p;
    std::string nm = p; size_t k = nm.find_last_of("\\/"); if (k != std::string::npos) nm = nm.substr(k + 1); k = nm.rfind('.'); if (k != std::string::npos) nm = nm.substr(0, k);
    return SceneLoad(nm, txt, p, lo);
  }
  if (t[1] == "line" && t.size() >= 3) {
    std::string body = Rest(line, 2); for (auto& ch : body) if (ch == '|') ch = '\n';
    return SceneLoad("line", body, "-");
  }
  return "err scene run <path> | line <step> | status | stop";
}
