#pragma once
#include "nbot_play.h"

static bool RdSeh(uintptr_t p, void* out, size_t n)
{
  __try { memcpy(out, (const void*)p, n); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
template <class T> static T Rd(uintptr_t p, T d = T()) { T v; return RdSeh(p, &v, sizeof v) ? v : d; }

static bool SidStr(uint32_t h, std::string& out)
{
  int32_t s = (int32_t)h; if (s == 0) return false;
  uint32_t i = s < 0 ? (uint32_t)~s : (uint32_t)s;
  uintptr_t pool = api->base + (s < 0 ? 0xb1407f0 : 0xb120760);
  uintptr_t chunk = Rd<uintptr_t>(pool + 8ull * (i >> 10)); if (!chunk) return false;
  uintptr_t e = chunk + (uintptr_t(i & 0x3ff) << 5);
  uintptr_t str = Rd<uintptr_t>(e); int32_t len = Rd<int32_t>(e + 8);
  if (!str || len <= 0 || len > 256) return false;
  wchar_t buf[257] = {}; if (!RdSeh(str, buf, size_t(len) * sizeof(wchar_t))) return false;
  out.clear(); for (int k = 0; k < len && buf[k]; ++k) { if (buf[k] > 0x7e) return false; out += (char)buf[k]; }
  return !out.empty();
}

static bool ReadWorld(npl::World& w, std::string& mainQ, std::string& err)
{
  w.clear(); mainQ.clear();
  if (!Nat().sm) { err = "ScenarioManager global not found by signature"; return false; } uintptr_t sm = Rd<uintptr_t>(Nat().sm); if (!sm) { err = "no ScenarioManager"; return false; }
  uintptr_t data = Rd<uintptr_t>(sm + 0x340); int n = Rd<int>(sm + 0x350);
  if (!data || n <= 0 || n > 20000) { err = Fmt("bad quest set data=0x%llx n=%d", (unsigned long long)data, n); return false; }
  for (int i = 0; i < n; i++) {
    uintptr_t e = data + (uintptr_t)i * 0x118; std::string q;
    if (!SidStr(Rd<uint32_t>(e), q) || !npl::ident(q)) continue;
    npl::QuestState& st = w[q];
    uintptr_t ad = Rd<uintptr_t>(e + 0x10); int an = Rd<int>(e + 0x18);
    if (ad && an > 0 && an < 5000) for (int k = 0; k < an; k++) { std::string s; if (SidStr(Rd<uint32_t>(ad + (uintptr_t)k * 0x18), s) && npl::node_of(q, s)) st.active.insert(s); }
    uintptr_t dd = Rd<uintptr_t>(e + 0xb0); int dn = Rd<int>(e + 0xb8);
    if (dd && dn > 0 && dn < 20000) for (int k = 0; k < dn; k++) { std::string s; if (SidStr(Rd<uint32_t>(dd + (uintptr_t)k * 0xc), s) && npl::node_of(q, s)) st.done.insert(s); }
  }
  std::string mq; if (SidStr(Rd<uint32_t>(sm + 0x410), mq) && w.count(mq)) mainQ = mq;
  return true;
}

static uint8_t LadderTopBlocked(uintptr_t lad) { uintptr_t c = Rd<uintptr_t>(lad + 0x358); return c ? Rd<uint8_t>(c + 0x10d, 2) : 2; }
static int OpenBit(uintptr_t a) { uintptr_t r = Rd<uintptr_t>(a + 0x2b8); return r ? (Rd<uint8_t>(r + 0x60, 0) & 1) : -1; }
static bool Climbing() { return RetVal(Call("pawn", "IsClimbing", "")) == "true"; }
static bool LadderSpan(uintptr_t lad, V3& loc, double& z0, double& z1)
{
  if (!ActorPos(lad, loc)) return false;
  V3 o{}, e{}; std::string bs = CallO(lad, "GetActorBounds", "false false");
  if (OutV(bs, "Origin", o) && OutV(bs, "BoxExtent", e) && e.z >= 100 && e.z <= 1500 && D2(o, loc) < 400) { z0 = o.z - e.z; z1 = o.z + e.z; return true; }
  char hb[64] = {0}; api->get(lad, "LadderHeight", hb, sizeof hb); double h = atof(hb); if (h < 100 || h > 3000) h = 400;
  z0 = loc.z; z1 = loc.z + h; return true;
}

enum PlaySt { PS_PICK, PS_NAV, PS_WAIT, PS_UNBLOCK, PS_OPEN_GO, PS_OPEN_TAP, PS_CLIMB, PS_END };
struct Play {
  bool on = false; int st = PS_PICK; std::string until, result = "idle", detail, mainQ; double timeout = 600;
  uint64_t t0 = 0, stT0 = 0, pollT = 0; npl::World w; bool wOk = false; int changes = 0;
  int stage = -1, unblocks = 0, navFails = 0, tries = 0; V3 target{}; bool partial = false; bool approach = false;
  uintptr_t ladder = 0, opener = 0; V3 lLoc{}; double lz0 = 0, lz1 = 0; bool down = true; int climbPh = 0; uint64_t climbT = 0, seenClimb = 0;
  std::set<uintptr_t> triedOpen; std::set<std::string> triedLadder;
  bool stepping = false, straight = false; int straightFails = 0, stepTps = 0, stallTps = 0, tp0 = 0; npl::StepProg sp; V3 aim{}; V3 lastStraight{};
};
static Play P;
static std::vector<npl::Stage> g_stages;
static std::string g_stagesPath;

static bool LoadStages()
{
  if (!g_stages.empty()) return true;
  std::string cands[] = { S2Path("plugins\\nbot\\quest_goals.tsv"), S2Path("bench\\quest_goals.tsv") };
  for (const std::string& c : cands) { std::string t; if (ReadFile2(c, t) && !t.empty()) { g_stages = npl::load_goals(t); g_stagesPath = c; break; } }
  return !g_stages.empty();
}

static void PlaySet(int st, const std::string& why) { Log("play %d -> %d %s", P.st, st, why.c_str()); P.st = st; P.stT0 = GetTickCount64(); P.tries = 0; }
static void PlayEnd(const char* res, const std::string& det)
{
  if (g.kind != G_NONE) Finish("aborted", "play end");
  SetMove(0, 0); P.result = res; P.detail = det; P.st = PS_END; P.on = false;
  Log("play %s %s (changes=%d teleports=%d secs=%.0f)", res, det.c_str(), P.changes, g_tpOk - P.tp0, (GetTickCount64() - P.t0) / 1000.0);
}

static int FindUntil() { for (size_t k = 0; k < g_stages.size(); k++) if (g_stages[k].stage == P.until) return (int)k; return -1; }

static void PlayPoll()
{
  uint64_t now = GetTickCount64(); if (now < P.pollT) return; P.pollT = now + 500;
  npl::World nw; std::string mq, err;
  if (!ReadWorld(nw, mq, err)) { Log("play quest read failed: %s", err.c_str()); return; }
  if (!P.wOk) {
    size_t na = 0, nd = 0; for (auto& kv : nw) { na += kv.second.active.size(); nd += kv.second.done.size(); }
    Log("play quest baseline quests=%zu active=%zu done=%zu main=%s", nw.size(), na, nd, mq.empty() ? "-" : mq.c_str());
    auto it = nw.find(mq.empty() ? std::string("E01_MQ01") : mq);
    if (it != nw.end()) { std::string a; for (auto& s : it->second.active) a += s + " "; Log("play quest %s active: %s", it->first.c_str(), a.c_str()); }
    P.w = nw; P.wOk = true; P.mainQ = mq; return;
  }
  for (auto& c : npl::diff(P.w, nw)) { Log("quest node %s %s state %d", c.quest.c_str(), c.node.c_str(), c.state); P.changes++; api->log("nbot quest node %s state %d", c.node.c_str(), c.state); }
  P.w = nw; if (!mq.empty()) P.mainQ = mq;
}

static std::string PlayStart(const std::vector<std::string>& t)
{
  if (!LoadStages()) return "err no quest_goals.tsv";
  if (Dead()) { Log("BENCH WARN play refused: pawn dead (GetHP 0)"); return "err pawn dead"; }
  P = Play(); P.on = true; P.t0 = P.stT0 = GetTickCount64(); P.result = "running"; P.tp0 = g_tpOk;
  P.until = nsc::opt(t, "until"); std::string to = nsc::opt(t, "timeout"); if (!to.empty()) P.timeout = atof(to.c_str());
  Log("play start until=%s timeout=%.0f stages=%zu from %s", P.until.c_str(), P.timeout, g_stages.size(), g_stagesPath.c_str());
  PlayPoll();
  return Fmt("play running until=%s stages=%zu", P.until.c_str(), g_stages.size());
}

static bool NavTo(V3 to, double tol, const std::string& seg, bool& partial)
{
  V3 p{}; if (!Pos(p)) return false; std::vector<V3> np; bool part = false;
  if (!NavPath(p, to, np, part)) { partial = true; Log("play nav fail seg=%s from=%.0f,%.0f,%.0f to=%.0f,%.0f,%.0f d=%.0f why=%s", seg.c_str(), p.x, p.y, p.z, to.x, to.y, to.z, D2(p, to), g_navWhy.c_str()); return false; }
  double ez = np.back().z - to.z; partial = part && (D2(np.back(), to) > 250 || fabs(ez) > 300);
  StartGoto(np, tol, 240, seg); g.nav = !partial; g.dest = to;
  Log("play nav seg=%s to=%.0f,%.0f,%.0f pts=%zu partial=%d end=%.0f,%.0f,%.0f", seg.c_str(), to.x, to.y, to.z, np.size(), partial ? 1 : 0, np.back().x, np.back().y, np.back().z);
  return true;
}

static bool ProjectNav(V3 q, V3& out)
{
  std::string r = Call("/Script/NavigationSystem.Default__NavigationSystemV1", "K2_ProjectPointToNavigation", Fmt("pawn %.1f,%.1f,%.1f null null 500,500,600", q.x, q.y, q.z).c_str());
  static int logged = 0; if (logged < 2) { logged++; Log("play project raw %s", OneLine(r, 200).c_str()); }
  return r.find("ReturnValue = true") != std::string::npos && OutV(r, "ProjectedLocation", out);
}

static bool StepToward(V3 p, V3 aim)
{
  P.aim = aim;
  if (!P.sp.update(D2(p, aim)) && P.sp.steps < 120 && P.stallTps < 4) {
    auto c0 = npl::step_candidates(npl::P3{ p.x, p.y, p.z }, npl::P3{ aim.x, aim.y, aim.z }, 3000);
    V3 qs{ c0[0].x, c0[0].y, c0[0].z };
    Log("shortcut: play stall teleport %.0f m toward %.0f,%.0f (no 5 m gain in %d steps, best=%.0f); upgrade when the step planner finds a path around this obstacle", D2(p, qs) / 100, aim.x, aim.y, P.sp.stale, P.sp.best);
    P.stallTps++; P.sp.stale = 0;
    if (SafeTeleport(qs, "stall-shortcut")) { P.stepping = true; P.straight = false; P.straightFails = 0; PlaySet(PS_NAV, "stall shortcut teleport"); return true; }
  }
  if (P.sp.stale >= 8 || P.sp.steps >= 120) { PlayEnd("failed", Fmt("step: no progress toward %.0f,%.0f,%.0f best=%.0f steps=%d", aim.x, aim.y, aim.z, P.sp.best, P.sp.steps)); return false; }
  auto cs = npl::step_candidates(npl::P3{ p.x, p.y, p.z }, npl::P3{ aim.x, aim.y, aim.z }, 3000);
  for (size_t i = 0; i < cs.size(); i++) {
    V3 q{ cs[i].x, cs[i].y, cs[i].z }, pr{}; bool pj = ProjectNav(q, pr) && npl::same_layer(cs[i], npl::P3{ pr.x, pr.y, pr.z }, 300);
    V3 to = pj ? pr : q; std::vector<V3> np; bool part = false;
    if (!NavPath(p, to, np, part)) continue;
    if (!npl::step_gain(npl::P3{ p.x, p.y, p.z }, npl::P3{ np.back().x, np.back().y, np.back().z }, npl::P3{ aim.x, aim.y, aim.z }, 500)) { Log("play step%zu path ends at %.0f,%.0f,%.0f (pts=%zu partial=%d): no gain", i, np.back().x, np.back().y, np.back().z, np.size(), part ? 1 : 0); continue; }
    StartGoto(np, 150, 90, Fmt("step%zu", i)); g.nav = !part; g.dest = to;
    P.stepping = true; P.straight = false; P.straightFails = 0; PlaySet(PS_NAV, Fmt("step %zu proj=%d pts=%zu partial=%d d=%.0f", i, pj ? 1 : 0, np.size(), part ? 1 : 0, D2(p, aim))); return true;
  }
  V3 q{ cs[7].x, cs[7].y, cs[7].z }, qs{ cs[0].x, cs[0].y, cs[0].z };
  if (P.straightFails >= 3) {
    Log("shortcut: play step teleport %.0f m toward %.0f,%.0f (no nav path in %zu candidates, straight walk stuck %d times); upgrade when nav tiles load around the player", D2(p, qs) / 100, aim.x, aim.y, cs.size(), P.straightFails);
    P.straightFails = 0; P.stepTps++;
    if (SafeTeleport(qs, "step-shortcut") || SafeTeleport(q, "step-shortcut")) { P.stepping = true; P.straight = false; PlaySet(PS_NAV, "shortcut teleport"); return true; }
  }
  StartGoto(std::vector<V3>{ q }, 150, 25, "straight"); g.allowTp = false; P.stepping = true; P.straight = true; P.lastStraight = p;
  PlaySet(PS_NAV, Fmt("straight: no nav path in %zu candidates", cs.size())); return true;
}

static void PlayTick()
{
  if (!P.on) return;
  uint64_t now = GetTickCount64(); double el = (now - P.stT0) / 1000.0;
  if ((now - P.t0) / 1000.0 > P.timeout) { PlayEnd("failed", Fmt("timeout in state %d", P.st)); return; }
  PlayPoll(); if (!P.wOk) return;
  int ui = FindUntil();
  if (!P.until.empty() && ui >= 0) {
    auto it = P.w.find(g_stages[ui].quest);
    if (it != P.w.end() && npl::stage_finished(g_stages[ui], it->second)) { PlayEnd("done", "stage " + P.until + " finished"); return; }
  }
  V3 p{}; Pos(p);
  if (P.st == PS_PICK) {
    npl::Pick pk = npl::pick_stage(g_stages, P.w, P.mainQ, p.x, p.y);
    if (pk.idx < 0) { if (el > 20) PlayEnd("failed", "no objective: " + pk.why); return; }
    const npl::Stage& s = g_stages[pk.idx];
    if (pk.idx != P.stage) { P.stage = pk.idx; P.unblocks = 0; P.navFails = 0; P.triedOpen.clear(); P.triedLadder.clear(); }
    P.target = V3{ s.gx, s.gy, s.gz };
    Log("play objective quest=%s stage=%s goal=%s %s at %.0f,%.0f,%.0f (%s)", s.quest.c_str(), s.stage.c_str(), s.kind.c_str(), s.goalSid.c_str(), s.gx, s.gy, s.gz, pk.why.c_str());
    bool part = false;
    P.approach = false; P.stepping = false;
    V3 aim = (s.mx != 0 || s.my != 0) ? V3{ s.mx, s.my, s.mz } : P.target;
    if (D2(p, aim) > 2500 && D2(p, P.target) > 2500) {
      std::vector<V3> np; bool pp = false;
      if (NavPath(p, P.target, np, pp) && !(pp && (D2(np.back(), P.target) > 250 || fabs(np.back().z - P.target.z) > 300))) { NavTo(P.target, s.kind == "trigger" ? 90 : 200, s.stage, part); P.partial = part; PlaySet(PS_NAV, "full path"); return; }
      Log("play far goal d=%.0f full path: %s", D2(p, P.target), g_navWhy.c_str());
      StepToward(p, aim); return;
    }
    if (!NavTo(P.target, s.kind == "trigger" ? 90 : 200, s.stage, part)) {
      if (++P.navFails > 3) { PlaySet(PS_UNBLOCK, "no nav path"); } return;
    }
    P.partial = part; PlaySet(PS_NAV, part ? "partial path" : "full path"); return;
  }
  const npl::Stage* cs = P.stage >= 0 ? &g_stages[P.stage] : nullptr;
  auto stageDone = [&]() { if (!cs) return false; auto it = P.w.find(cs->quest); return it != P.w.end() && npl::stage_finished(*cs, it->second); };
  if (P.st == PS_NAV) {
    if (stageDone()) { if (g.kind != G_NONE) Finish("aborted", "stage done"); PlaySet(PS_PICK, "stage finished on the way"); return; }
    if (g.kind != G_NONE) return;
    if (g_set.on) { TickSettle(); return; }
    if (P.stepping) {
      if (P.straight && D2(p, P.lastStraight) < 100) { Log("play straight walk did not move the pawn (%.0f cm): movement blocked", D2(p, P.lastStraight)); P.straightFails = 3; }
      else if (P.straight && (g.state != "done" || D2(p, P.lastStraight) < 500)) P.straightFails++;
      P.stepping = false; P.navFails = 0; PlaySet(PS_PICK, Fmt("step %s at %.0f,%.0f,%.0f d=%.0f straightFails=%d", g.state.c_str(), p.x, p.y, p.z, D2(p, P.aim), P.straightFails)); return;
    }
    if (g.state == "done" && !P.partial) { PlaySet(PS_WAIT, "at goal"); return; }
    PlaySet(PS_UNBLOCK, Fmt("nav %s partial=%d", g.state.c_str(), P.partial ? 1 : 0)); return;
  }
  if (P.st == PS_WAIT) {
    if (stageDone()) { PlaySet(PS_PICK, "stage finished"); return; }
    if (el > 6) PlaySet(PS_UNBLOCK, "at goal but the stage did not end");
    return;
  }
  if (P.st == PS_UNBLOCK) {
    if (++P.unblocks > 4) { PlayEnd("failed", Fmt("stuck: %d unblock attempts for %s", P.unblocks - 1, cs ? cs->stage.c_str() : "-")); return; }
    uintptr_t best = 0; double bd = 3000; V3 bl{};
    for (auto a : FindAll("isa:VerticalLadder", 400)) { V3 l; if (!ActorPos(a, l)) continue; double d = D2(l, P.target); if (d < bd && !P.triedLadder.count(Fmt("%llx", (unsigned long long)a))) { bd = d; best = a; bl = l; } }
    if (best) {
      P.ladder = best; LadderSpan(best, P.lLoc, P.lz0, P.lz1); P.down = p.z > (P.lz0 + P.lz1) / 2;
      uint8_t tb = LadderTopBlocked(best);
      Log("play ladder 0x%llx at %.0f,%.0f span z %.0f..%.0f (%.0f cm from goal) topBlocked=%d enter=%s", (unsigned long long)best, P.lLoc.x, P.lLoc.y, P.lz0, P.lz1, bd, tb, P.down ? "top" : "bottom");
      if (tb == 1 && P.down) {
        uintptr_t ob = 0; double od = 700; V3 top{ P.lLoc.x, P.lLoc.y, P.lz1 };
        for (auto a : FindAll("isa:InteractableObject", 4000)) {
          if (a == best || P.triedOpen.count(a)) continue; V3 l; if (!ActorPos(a, l)) continue;
          double d = sqrt((l.x - top.x) * (l.x - top.x) + (l.y - top.y) * (l.y - top.y) + (l.z - top.z) * (l.z - top.z));
          if (d < od) { od = d; ob = a; }
        }
        if (!ob) { PlayEnd("failed", "ladder top blocked and no interactable near its top"); return; }
        P.opener = ob; V3 ol{}; ActorPos(ob, ol);
        std::string nm = Call(Fmt("0x%llx", (unsigned long long)ob).c_str(), "GetName", "");
        Log("play opener 0x%llx %s at %.0f,%.0f,%.0f (%.0f cm from the ladder top) open=%d", (unsigned long long)ob, RetVal(nm).c_str(), ol.x, ol.y, ol.z, od, OpenBit(ob));
        double sx, sy; nsc::stand_point(ol.x, ol.y, p.x, p.y, 110, sx, sy); bool part = false;
        if (!NavTo(V3{ sx, sy, ol.z }, 60, "opener", part)) StartGoto(std::vector<V3>{ V3{ sx, sy, ol.z } }, 60, 60, "opener");
        PlaySet(PS_OPEN_GO, "to opener"); return;
      }
      P.climbPh = 0; PlaySet(PS_CLIMB, "ladder link"); return;
    }
    PlayEnd("failed", "goal unreachable and no ladder within 30 m"); return;
  }
  if (P.st == PS_OPEN_GO) { if (g.kind != G_NONE) return; PlaySet(PS_OPEN_TAP, "at opener " + g.state); return; }
  if (P.st == PS_OPEN_TAP) {
    V3 ol{}; if (!ActorPos(P.opener, ol)) { PlaySet(PS_UNBLOCK, "opener gone"); return; }
    if (LadderTopBlocked(P.ladder) == 0) { Log("play ladder top unblocked (opener open=%d)", OpenBit(P.opener)); P.climbPh = 0; PlaySet(PS_CLIMB, "ladder link"); return; }
    if (g.kind == G_HOLD) return;
    if (el > 12) { P.triedOpen.insert(P.opener); PlaySet(PS_UNBLOCK, "opener did not unblock the ladder"); return; }
    if (now < P.climbT) return;
    static const double pitchOff[] = { 0, -25, 20, -45, 40, -10 };
    double off = pitchOff[P.tries % 6];
    LookAt(p, V3{ ol.x, ol.y, ol.z + off });
    std::string ia = Call("pawn", "GetInteractedActor", ""); std::string want = Fmt("0x%llx", (unsigned long long)P.opener);
    bool aimed = ia.find(want) != std::string::npos;
    if (aimed && P.tries < 8) { std::string r = Tap(); Log("play usable opener (stock target) -> %s", r.c_str()); P.climbT = now + 2500; P.tries += 8; return; }
    if (aimed) { Exec("hold 1500"); Log("play hold on opener"); P.climbT = now + 2500; return; }
    Log("play aim try %d pitch off %.0f: interacted=%s", P.tries, off, RetVal(ia).c_str()); P.tries++; P.climbT = now + 600; return;
  }
  if (P.st == PS_CLIMB) {
    uintptr_t pw = Find("pawn"); uintptr_t cl = Rd<uintptr_t>(pw + 0x9e8); uintptr_t C = cl ? cl + 0x2d8 : 0;
    bool on = Climbing();
    if (P.climbPh == 0) {
      double tz = P.down ? P.lz1 : P.lz0;
      if (D2(p, P.lLoc) > 220 || fabs(p.z - tz) > 250) {
        if (g.kind == G_NONE && P.tries < 2) { P.tries++; double sx, sy; nsc::stand_point(P.lLoc.x, P.lLoc.y, p.x, p.y, 120, sx, sy); StartGoto(std::vector<V3>{ V3{ sx, sy, tz } }, 70, 40, "ladder"); }
        if (g.kind != G_NONE && el < 45) return;
      }
      if (g.kind != G_NONE) Finish("aborted", "ladder reach");
      std::string r = Call("pawn", "SetClimbTarget", Fmt("0x%llx", (unsigned long long)P.ladder).c_str());
      Log("play SetClimbTarget 0x%llx -> %s", (unsigned long long)P.ladder, r.substr(0, 60).c_str());
      P.climbPh = 1; P.climbT = now; P.seenClimb = 0; return;
    }
    if (P.climbPh == 1) {
      if (on) { if (!P.seenClimb) { P.seenClimb = now; Log("play climbing (C=0x%llx)", (unsigned long long)C); } if (C) NatSeh(api->base + (P.down ? 0x6836912 : 0x7eb63c), C); }
      if (P.seenClimb && !on) { Log("play off the ladder at %.0f,%.0f,%.0f", p.x, p.y, p.z); PlaySet(PS_PICK, "ladder done"); return; }
      if (!P.seenClimb && now - P.climbT > 1500 && now - P.climbT < 2500) SetMove(1, 0);
      if (!P.seenClimb && now - P.climbT >= 2500 && P.tries < 3) { SetMove(0, 0); P.tries++; P.climbPh = 0; return; }
      if ((!P.seenClimb && P.tries >= 3) || now - P.climbT > 25000) {
        SetMove(0, 0); P.triedLadder.insert(Fmt("%llx", (unsigned long long)P.ladder));
        V3 dst{ P.lLoc.x, P.lLoc.y, (P.down ? P.lz0 : P.lz1) + 60 };
        Log("BENCH WARN ladder climb not driven (seen=%d), teleport link to the other end", P.seenClimb ? 1 : 0);
        if (on) Call("pawn", "SetClimbTarget", "null");
        if (!SafeTeleport(dst, "ladder-link")) { PlayEnd("failed", "ladder link: no floor at the other end"); return; }
        P.climbPh = 2; return;
      }
      return;
    }
    if (P.climbPh == 2) { if (g_set.on) { TickSettle(); return; } PlaySet(PS_PICK, "ladder link by teleport"); return; }
  }
}

static std::string PlayJson()
{
  const char* sn = P.stage >= 0 && P.stage < (int)g_stages.size() ? g_stages[P.stage].stage.c_str() : "-";
  return Fmt("{\"play\":\"%s\",\"st\":%d,\"stage\":\"%s\",\"until\":\"%s\",\"changes\":%d,\"teleports\":%d,\"main\":\"%s\",\"detail\":\"%s\",\"secs\":%.0f}", P.result.c_str(), P.st, sn, P.until.c_str(), P.changes, P.on || P.t0 ? g_tpOk - P.tp0 : 0, P.mainQ.c_str(), nsc::jesc(P.detail).c_str(), P.t0 ? (GetTickCount64() - P.t0) / 1000.0 : 0.0);
}

static std::string QuestDump(const std::string& q)
{
  npl::World w; std::string mq, err; if (!ReadWorld(w, mq, err)) return "err " + err;
  std::string o = Fmt("main=%s quests=%zu", mq.c_str(), w.size());
  auto it = w.find(q.empty() ? (mq.empty() ? std::string("E01_MQ01") : mq) : q); if (it == w.end()) return o + " (quest not found)";
  o += "\nactive:"; for (auto& s : it->second.active) o += " " + s;
  o += "\ndone:"; for (auto& s : it->second.done) o += " " + s;
  return o;
}
