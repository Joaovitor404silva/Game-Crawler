static qr::Ring g_qring(2000);
static std::mutex g_qm;
static bool g_qtap, g_qprevOk;
static std::vector<qst::Quest> g_qprev;
static uint64_t g_qlast;
static const qst::QLayout QL;

static bool QRdFn(uint64_t a, void* o, size_t n) { return SafeRead(o, (uintptr_t)a, n); }

static uintptr_t QPool() { static uintptr_t a; if (!a) a = SigFind(H->base, SIG_NAMEPOOL, SIG_NAMEPOOL_RIP); return a; }

static bool QSid(uint32_t h, std::string& out) {
  static std::mutex m; static std::unordered_map<uint32_t, std::string> memo;
  { std::lock_guard<std::mutex> l(m); auto it = memo.find(h); if (it != memo.end()) { out = it->second; return !out.empty(); } }
  std::string r; qst::PoolSlot ps;
  uintptr_t p0 = QPool();
  if (p0 && qst::PoolSlotOf(h, ps)) {
    uintptr_t pool = p0 + (ps.neg ? qst::kNegPoolDelta : 0);
    uintptr_t chunk = RdP(pool + 8ull * ps.chunk);
    if (chunk) {
      uintptr_t e = chunk + ps.entry;
      uintptr_t str = RdP(e); int32_t len = Rd<int32_t>(e + 8);
      if (str && len > 0 && len <= 256) {
        wchar_t buf[257] = {};
        if (SafeRead(buf, str, size_t(len) * sizeof(wchar_t))) {
          bool good = true; std::string tmp;
          for (int k = 0; k < len && buf[k]; ++k) { if (buf[k] > 0x7e) { good = false; break; } tmp += (char)buf[k]; }
          if (good) r = tmp;
        }
      }
    }
  }
  if (!r.empty()) { std::lock_guard<std::mutex> l(m); memo[h] = r; }
  out = r; return !r.empty();
}

static uintptr_t QSmGlobal() { static uintptr_t a; if (!a) a = SigFind(H->base, SIG_SM, SIG_SM_RIP); return a; }

static bool QRead(std::vector<qst::Quest>& qs, std::string& err, bool check = true) {
  uintptr_t g = QSmGlobal(); uintptr_t sm = g ? RdP(g) : 0;
  if (!sm) { err = "ScenarioManager not found"; return false; }
  if (!qst::ReadAll(QRdFn, sm, QL, qs, err)) return false;
  if (check) { std::string why; if (!qst::Sane(qs, QSid, why)) { err = "quest layout check failed: " + why; return false; } }
  return true;
}

void QuestTick() {
  if (!g_qtap) return;
  uint64_t now = GetTickCount64();
  if (now - g_qlast < 200) return;
  g_qlast = now;
  std::vector<qst::Quest> cur; std::string err;
  if (!QRead(cur, err)) return;
  std::lock_guard<std::mutex> l(g_qm);
  if (g_qprevOk) {
    uint64_t s0 = g_qring.Seq();
    qst::Diff(g_qprev, cur, QSid, g_qring, now);
    for (auto& e : g_qring.Since(s0)) LOG("qev %llu %s %s %s", (unsigned long long)e.ms, e.kind.c_str(), e.guid.c_str(), e.sid.c_str());
  }
  g_qprev.swap(cur); g_qprevOk = true;
}

static std::string QNodeJson(const qst::NodeInfo& n, const std::string& sid) {
  return jo::Obj().str("node", sid).str("state", n.state).str("quest", n.quest).hex("obj", n.obj).num("active", n.active).num("finished", n.finished).done();
}

static std::string QStateLine(const std::vector<qst::Quest>& qs, const std::string& sid, std::string* json) {
  qst::NodeInfo n = qst::NodeState(qs, QSid, sid);
  if (json) *json = QNodeJson(n, sid);
  if (n.state == "none") return sid + ": none";
  std::string s = sid + ": " + n.state + " (quest " + n.quest + ": " + std::to_string(n.active) + " active, " + std::to_string(n.finished) + " finished";
  if (n.obj) s += Fmt("; obj 0x%llx %s", (unsigned long long)n.obj, IsObj(n.obj) ? ClassName(n.obj).c_str() : "?");
  return s + ")";
}

static void CmdQuestState(const std::string& sid, Out& o) {
  std::vector<qst::Quest> qs; std::string err;
  if (!QRead(qs, err)) { o.line("error: " + err); return; }
  const qst::Quest* q = qst::FindQuest(qs, QSid, sid);
  qst::NodeInfo n = qst::NodeState(qs, QSid, sid);
  if (n.state == "none" && q) {
    std::vector<std::string> js;
    o.f("%s: quest, %zu active, %zu finished", sid.c_str(), q->active.size(), q->done.size());
    for (auto& a : q->active) {
      std::string ns; QSid(a.h, ns);
      std::string cls = IsObj(a.obj) ? ClassName(a.obj) : "?";
      o.f("  active %s obj=0x%llx %s", ns.c_str(), (unsigned long long)a.obj, cls.c_str());
      js.push_back(jo::Obj().str("sid", ns).hex("obj", a.obj).str("class", cls).done());
    }
    JsonSetData(jo::Obj().str("quest", sid).str("state", "quest").num("finished", (long long)q->done.size()).raw("nodes", jo::Arr(js)).done());
    return;
  }
  std::string js; o.line(QStateLine(qs, sid, &js)); JsonSetData(js);
}

static void CmdQuestList(const std::vector<std::string>& t, Out& o) {
  uopt::Args ao = uopt::Parse(t, 2); std::string f = Lower(ao.Str("filter"));
  std::vector<qst::Quest> qs; std::string err;
  if (!QRead(qs, err)) { o.line("error: " + err); return; }
  std::vector<std::string> js; int shown = 0;
  for (auto& q : qs) {
    std::string s; QSid(q.h, s); if (s.empty()) continue;
    if (!f.empty() && Lower(s).find(f) == std::string::npos) continue;
    o.f("%s active=%zu finished=%zu", s.c_str(), q.active.size(), q.done.size());
    js.push_back(jo::Obj().str("sid", s).num("active", (long long)q.active.size()).num("finished", (long long)q.done.size()).done()); shown++;
  }
  JsonSetData(jo::Obj().raw("quests", jo::Arr(js)).num("total", (long long)qs.size()).done());
  o.f("%d of %zu quests", shown, qs.size());
}

static void CmdQuestLayout(Out& o) {
  uintptr_t g = QSmGlobal();
  o.f("sm global %s rva 0x%llx", g ? "found" : "NOT FOUND", (unsigned long long)(g ? g - H->base : 0));
  o.f("build-specific: arr +0x%x num +0x%x stride 0x%x sid +0x%x | active +0x%x num +0x%x stride 0x%x obj +0x%x | done +0x%x num +0x%x stride 0x%x | name pool rva 0x%llx (+0x%llx)", QL.arr, QL.num, QL.stride, QL.sid, QL.act, QL.actNum, QL.actStride, QL.actObj, QL.done, QL.doneNum, QL.doneStride, (unsigned long long)(QPool() ? QPool() - H->base : 0), (unsigned long long)qst::kNegPoolDelta);
  std::vector<qst::Quest> qs; std::string err;
  if (!QRead(qs, err)) { o.line("sanity: FAIL " + err); JsonSetData(jo::Obj().b("sane", false).str("error", err).done()); return; }
  size_t act = 0, dn = 0; for (auto& q : qs) { act += q.active.size(); dn += q.done.size(); }
  o.f("sanity: ok, %zu quests, %zu active nodes, %zu finished nodes", qs.size(), act, dn);
  size_t tot = 0, res = 0, ntot = 0, nres = 0;
  int shown = 0;
  auto probe = [&](uint32_t h) {
    if (h == 0xffffffffu) return;
    std::string s; bool ok = QSid(h, s); tot++; res += ok;
    if ((int32_t)h < 0) { ntot++; nres += ok; }
    if (!ok && shown < 3) { shown++; o.f("unresolved handle 0x%x", h); }
  };
  for (auto& q : qs) { probe(q.h); for (auto& a : q.active) probe(a.h); for (uint32_t h : q.done) probe(h); }
  o.f("handles: %zu resolved of %zu; negative pool %zu of %zu", res, tot, nres, ntot);
  JsonSetData(jo::Obj().b("sane", true).num("quests", (long long)qs.size()).num("active", (long long)act).num("finished", (long long)dn).num("handles", (long long)tot).num("resolved", (long long)res).num("neg", (long long)ntot).num("neg_resolved", (long long)nres).done());
}

static void CmdQuestTap(const std::vector<std::string>& t, Out& o) {
  std::string v = t.size() > 2 ? Lower(t[2]) : "";
  uopt::Args ao = uopt::Parse(t, 3);
  if (v == "on") {
    std::vector<qst::Quest> qs; std::string err;
    if (!QRead(qs, err)) { o.line("error: " + err); return; }
    { std::lock_guard<std::mutex> l(g_qm); g_qprev.swap(qs); g_qprevOk = true; g_qlast = 0; g_qtap = true; }
    o.f("quest tap on, mark %llu", (unsigned long long)g_qring.Seq()); JsonSetData(jo::Obj().b("tap", true).num("mark", (long long)g_qring.Seq()).done()); return;
  }
  if (v == "off") { g_qtap = false; o.f("quest tap off, mark %llu", (unsigned long long)g_qring.Seq()); JsonSetData(jo::Obj().b("tap", false).num("mark", (long long)g_qring.Seq()).done()); return; }
  if (v == "clear") { g_qring.Clear(); o.line("quest tap cleared"); return; }
  if (v == "show") {
    auto ev = g_qring.Since((uint64_t)ao.Int("since", 0), ao.Str("sid"), (size_t)ao.Int("max", 200));
    std::vector<std::string> js;
    for (auto& e : ev) {
      o.f("qev %llu %s %s %s (#%llu)", (unsigned long long)e.ms, e.kind.c_str(), e.guid.c_str(), e.sid.c_str(), (unsigned long long)e.seq);
      js.push_back(jo::Obj().num("seq", (long long)e.seq).num("ms", (long long)e.ms).str("kind", e.kind).str("quest", e.guid).str("sid", e.sid).done());
    }
    JsonSetData(jo::Obj().b("tap", g_qtap).num("mark", (long long)g_qring.Seq()).raw("events", jo::Arr(js)).done());
    o.f("%zu events, mark %llu", ev.size(), (unsigned long long)g_qring.Seq()); return;
  }
  o.line("usage: quest tap on|off|clear|show [since=<mark>] [sid=<substr>] [max=N]");
}

static void CmdQuestMove(const std::string& verb, const std::string& sid, Out& o) {
  std::vector<qst::Quest> qs; std::string err;
  if (!QRead(qs, err)) { o.line("error: " + err); return; }
  o.line("before " + QStateLine(qs, sid, nullptr));
  uintptr_t mgr = FindFirst([&](uintptr_t x) { return ClassName(x) == "CustomConsoleManagerAA" && !IsDefault(x); });
  if (!mgr) { o.line("error: CustomConsoleManagerAA instance not found"); return; }
  std::vector<PCall> pc(1);
  if (!PrepCall(mgr, verb == "end" ? "XEndQuestNodeBySID" : "XStartQuestNodeBySID", { sid }, pc[0], err) || !RunCalls(pc, err)) { o.line("error: " + err); return; }
  std::vector<qst::Quest> after;
  if (!QRead(after, err)) { o.line("error: " + err); return; }
  std::string js; o.line("after  " + QStateLine(after, sid, &js)); JsonSetData(js);
}

static void CmdQuest(const std::vector<std::string>& t, Out& o) {
  std::string v = t.size() > 1 ? Lower(t[1]) : "";
  if (v == "list") { CmdQuestList(t, o); return; }
  if (v == "layout") { CmdQuestLayout(o); return; }
  if (v == "tap") { CmdQuestTap(t, o); return; }
  if ((v == "state" || v == "end" || v == "start") && t.size() > 2) {
    if (v == "state") CmdQuestState(t[2], o); else CmdQuestMove(v, t[2], o);
    return;
  }
  o.line("usage: quest list [filter=<substr>] | state <QuestSID|NodeSID> | tap on|off|clear|show [since=<mark>] [sid=<substr>] | end <NodeSID> | start <NodeSID> | layout");
}
