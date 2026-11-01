#include "core.h"
#include "home.h"
#include "minhook/include/MinHook.h"
#include "agg.h"
#include "stackwalk.h"
#include "hwhook.h"
#include "json_out.h"
#include <tlhelp32.h>
#include <map>
#include <mutex>
#include <algorithm>
#include <fstream>
#include <sstream>

std::string ObjName(uintptr_t o);
bool IsObj(uintptr_t p);

struct Native { std::string name; uint64_t exec, impl; };
static std::vector<Native> g_nat;
static std::map<uint64_t, size_t> g_natByRva;
void LoadNatives() {
  g_nat.clear(); g_natByRva.clear();
  std::ifstream f(S2Env("ANOMALY_NATIVES", S2Path("names\\natives_impl.tsv"))); std::string line;
  while (std::getline(f, line)) {
    std::istringstream ss(line); std::string n, e, i; std::getline(ss, n, '\t'); std::getline(ss, e, '\t'); std::getline(ss, i, '\t');
    uint64_t ev = _strtoui64(e.c_str(), nullptr, 16), iv = _strtoui64(i.c_str(), nullptr, 16);
    if (!ev) continue; g_nat.push_back({ n, ev - 0x140000000, iv ? iv - 0x140000000 : 0 });
    g_natByRva[g_nat.back().exec] = g_nat.size() - 1; if (iv) g_natByRva.emplace(g_nat.back().impl, g_nat.size() - 1);
  }
  LOG("natives loaded: %zu", g_nat.size());
}
std::string NativeNameAt(uintptr_t a) {
  if (g_natByRva.empty()) return "";
  uint64_t r = a - H->base; auto it = g_natByRva.upper_bound(r); if (it == g_natByRva.begin()) return "";
  --it; uint64_t d = r - it->first; if (d > 0x4000) return "";
  const Native& n = g_nat[it->second]; bool impl = it->first == n.impl && n.impl != n.exec;
  return d ? Fmt("~%s%s+0x%llx", n.name.c_str(), impl ? "(impl)" : "(exec)", d) : Fmt("%s%s", n.name.c_str(), impl ? "(impl)" : "(exec)");
}
bool LookupNative(const std::string& name, uintptr_t& exec, uintptr_t& impl) {
  std::string ln = Lower(name);
  for (auto& n : g_nat) if (Lower(n.name) == ln) { exec = H->base + n.exec; impl = n.impl ? H->base + n.impl : 0; return true; }
  return false;
}

struct Filter {
  bool has[4] = {}; uint64_t v[4] = {};
  bool any() const { return has[0] || has[1] || has[2] || has[3]; }
  bool match(uint64_t a, uint64_t b, uint64_t c, uint64_t d) const {
    return (!has[0] || v[0] == a) && (!has[1] || v[1] == b) && (!has[2] || v[2] == c) && (!has[3] || v[3] == d);
  }
};
struct SrcOpts {
  bool agg = false; int depth = 0; int maxLog = 0; Filter fl;
  int secs = 0; bool wait = false; int slot = -1; bool hw = false;
};
static const int kDefaultDepth = 12, kDefaultMaxAgg = 40;

static bool ParseSrcOpts(const std::vector<std::string>& t, size_t from, SrcOpts& o, std::string& err, std::vector<std::string>* rest) {
  static const char* regs[] = { "rcx=", "rdx=", "r8=", "r9=" };
  for (size_t i = from; i < t.size(); i++) {
    const std::string& s = t[i]; bool took = false;
    if (s == "agg") { o.agg = true; took = true; }
    else if (s == "wait") { o.wait = true; took = true; }
    else if (s == "hw") { o.hw = true; took = true; }
    else if (s == "stack") { o.depth = kDefaultDepth; took = true; }
    else if (s.rfind("stack=", 0) == 0) { o.depth = std::min(std::max(atoi(s.c_str() + 6), 1), agg::MAXF); took = true; }
    else if (s.rfind("max=", 0) == 0) { o.maxLog = std::max(atoi(s.c_str() + 4), 0); took = true; }
    else if (s.rfind("secs=", 0) == 0) { o.secs = std::min(std::max(atoi(s.c_str() + 5), 1), 3600); took = true; }
    else if (s.rfind("slot=", 0) == 0) { o.slot = atoi(s.c_str() + 5); took = true; }
    else for (int k = 0; k < 4 && !took; k++) if (s.rfind(regs[k], 0) == 0) {
      uint64_t v; std::string e; if (!EvalAddr(s.substr(strlen(regs[k])), v, e)) { err = std::string(regs[k]) + e; return false; }
      o.fl.has[k] = true; o.fl.v[k] = v; took = true;
    }
    if (!took && rest) rest->push_back(s);
  }
  if (o.agg && o.depth == 0) o.depth = kDefaultDepth;
  if (o.agg && o.maxLog == 0) o.maxLog = kDefaultMaxAgg;
  return true;
}

static std::string GroupLine(const char* src, int idx, const agg::Group& x, const char* kind) {
  std::string stack = agg::StackText(x, [](uint64_t a, bool r) { return FrameSym(a, r); });
  if (!strcmp(kind, "write") || !strcmp(kind, "rw"))
    return Fmt("grp %s g%d hits=%lld tid=%u at=0x%llx old=0x%llx new=0x%llx stack=%s", src, idx, (long long)x.hits.load(), x.tid, x.where, x.firstOld, x.firstNew, stack.c_str());
  if (!strcmp(kind, "exec"))
    return Fmt("grp %s g%d hits=%lld tid=%u stack=%s", src, idx, (long long)x.hits.load(), x.tid, stack.c_str());
  return Fmt("grp %s g%d hits=%lld tid=%u rcx=0x%llx rdx=0x%llx stack=%s", src, idx, (long long)x.hits.load(), x.tid, x.firstOld, x.firstNew, stack.c_str());
}
static std::string GroupJson(int idx, const agg::Group& x, const char* kind) {
  std::vector<std::string> st, raw;
  for (int i = 0; i < x.nframes; i++) { st.push_back(FrameSym(x.frames[i], i > 0)); raw.push_back(jo::Hex(x.frames[i])); }
  jo::Obj o; o.num("id", idx).num("hits", x.hits.load()).num("tid", x.tid).str("top", st.empty() ? "" : st[0]).raw("stack", jo::StrArr(st)).raw("frames", jo::Arr(raw));
  if (!strcmp(kind, "write") || !strcmp(kind, "rw")) o.hex("at", x.where).hex("old", x.firstOld).hex("new", x.firstNew).hex("last_old", x.lastOld.load()).hex("last_new", x.lastNew.load());
  else if (!strcmp(kind, "hook")) o.hex("rcx", x.firstOld).hex("rdx", x.firstNew);
  o.num("first_tick", (long long)x.firstTick).num("last_tick", (long long)x.lastTick.load());
  return o.done();
}
static void ReportGroups(agg::Table* tab, const char* kind, int topN, std::vector<std::string>& lines, std::string& jsonArr) {
  std::vector<std::string> js;
  for (int gi : tab->Top(topN)) {
    const agg::Group& x = tab->g[gi];
    std::string top = FrameSym(x.frames[0], false);
    std::string callers; for (int i = 1; i < x.nframes; i++) callers += (i > 1 ? " < " : "") + FrameSym(x.frames[i], true);
    if (!strcmp(kind, "write") || !strcmp(kind, "rw"))
      lines.push_back(Fmt("  g%-3d %7lld x  %s  old=0x%llx new=0x%llx at=0x%llx tid=%u\n         callers: %s", gi, (long long)x.hits.load(), top.c_str(), x.firstOld, x.firstNew, x.where, x.tid, callers.c_str()));
    else if (!strcmp(kind, "exec"))
      lines.push_back(Fmt("  g%-3d %7lld x  %s tid=%u\n         callers: %s", gi, (long long)x.hits.load(), top.c_str(), x.tid, callers.c_str()));
    else
      lines.push_back(Fmt("  g%-3d %7lld x  rcx=0x%llx rdx=0x%llx tid=%u\n         caller chain: %s", gi, (long long)x.hits.load(), x.firstOld, x.firstNew, x.tid, callers.c_str()));
    js.push_back(GroupJson(gi, x, kind));
  }
  jsonArr = jo::Arr(js);
}

struct Hook {
  int id; uintptr_t target; void* tramp; uint8_t* stub; int mode;
  uint64_t retval; volatile LONG64 hits; std::string label; DWORD lastSec; int secHits;
  SrcOpts o; agg::Table* tab; volatile LONG64 logged, filtered; std::string owner;
};
static std::mutex g_hm;
static std::vector<Hook*> g_hooks; static int g_hookId;
static std::vector<void*> g_stubPages;

static std::string ArgDesc(uint64_t v) {
  std::string d = IsObj(v) ? " (" + ObjName(v) + ")" : ""; return Fmt("0x%llx%s", v, d.c_str());
}
extern "C" int HookOnHit(Hook* h, uint64_t* regs) {
  if (h->o.fl.any() && !h->o.fl.match(regs[3], regs[2], regs[1], regs[0])) { InterlockedIncrement64(&h->filtered); return 0; }
  LONG64 n = InterlockedIncrement64(&h->hits);
  if (h->tab) {
    uint64_t fr[agg::MAXF]; int nf = WalkFromEntry(h->target, &regs[4], fr, h->o.depth);
    bool isNew = false; int gi = h->tab->Hit(fr, nf, 0, 0, regs[3], regs[2], GetCurrentThreadId(), H->tickCount, &isNew);
    if (isNew && gi >= 0 && (h->o.maxLog <= 0 || InterlockedIncrement64(&h->logged) <= h->o.maxLog))
      LOG("%s", GroupLine(Fmt("hook%d", h->id).c_str(), gi, h->tab->g[gi], "hook").c_str());
    return h->mode;
  }
  DWORD sec = GetTickCount() / 1000;
  if (sec != h->lastSec) { h->lastSec = sec; h->secHits = 0; }
  if (++h->secHits <= 10) {
    std::string st;
    if (h->o.depth > 0) { uint64_t fr[agg::MAXF]; int nf = WalkFromEntry(h->target, &regs[4], fr, h->o.depth); for (int i = 1; i < nf; i++) st += (i > 1 ? " < " : " stack=") + FrameSym(fr[i], true); }
    LOG("hit #%lld hook %d %s tid=%lu rcx=%s rdx=%s r8=%s r9=%s caller=%s%s", n, h->id, h->label.c_str(), GetCurrentThreadId(),
        ArgDesc(regs[3]).c_str(), ArgDesc(regs[2]).c_str(), ArgDesc(regs[1]).c_str(), ArgDesc(regs[0]).c_str(), Sym(regs[4]).c_str(), st.c_str());
  }
  return h->mode;
}

static uint8_t* AllocStub(size_t n) {
  static uint8_t* page; static size_t used = 4096;
  if (used + n > 4096) { page = (uint8_t*)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE); used = 0; g_stubPages.push_back(page); }
  uint8_t* p = page + used; used += (n + 15) & ~15; return p;
}
static uint8_t* BuildStub(Hook* h) {
  uint8_t* s = AllocStub(160), *p = s;
  auto B = [&](std::initializer_list<uint8_t> b) { for (auto x : b) *p++ = x; };
  auto Q = [&](uint64_t v) { memcpy(p, &v, 8); p += 8; };
  B({ 0x51, 0x52, 0x41, 0x50, 0x41, 0x51 });
  B({ 0x48, 0x83, 0xEC, 0x68 });
  B({ 0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x20, 0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x30, 0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x40, 0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x50 });
  B({ 0x48, 0x8D, 0x54, 0x24, 0x68 });
  B({ 0x48, 0xB9 }); Q((uint64_t)h);
  B({ 0x48, 0xB8 }); Q((uint64_t)&HookOnHit);
  B({ 0xFF, 0xD0 });
  B({ 0x41, 0x89, 0xC3 });
  B({ 0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x20, 0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x30, 0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x40, 0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x50 });
  B({ 0x48, 0x83, 0xC4, 0x68 });
  B({ 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59 });
  B({ 0x45, 0x85, 0xDB });
  B({ 0x75, 0x0C });
  B({ 0x48, 0xB8 }); uint8_t* trampSlot = p; Q(0);
  B({ 0xFF, 0xE0 });
  B({ 0x48, 0xB8 }); Q((uint64_t)&h->retval);
  B({ 0x48, 0x8B, 0x00, 0xC3 });
  h->stub = s; *(uint8_t**)&h->tramp = trampSlot;
  return s;
}

uintptr_t DetourTarget(uintptr_t a) {
  uint8_t b[14]; if (!SafeRead(b, a, sizeof b)) return 0;
  if (b[0] == 0xE9) { int32_t rel; memcpy(&rel, b + 1, 4); return a + 5 + rel; }
  if (b[0] == 0xFF && b[1] == 0x25) { int32_t rel; memcpy(&rel, b + 2, 4); return RdP(a + 6 + rel); }
  if (b[0] == 0x48 && b[1] == 0xB8 && b[10] == 0xFF && b[11] == 0xE0) { uint64_t v; memcpy(&v, b + 2, 8); return v; }
  return 0;
}
std::string DetourOwner(uintptr_t a) {
  uintptr_t t = DetourTarget(a);
  return t && !InModule(t) ? ModOf(t) : "";
}

static std::string AddHook(uintptr_t target, int mode, uint64_t rv, const std::string& label, const SrcOpts& so) {
  std::lock_guard<std::mutex> l(g_hm);
  for (auto* x : g_hooks) if (x->target == target) return Fmt("already hooked as %d", x->id);
  Hook* h = new Hook{}; h->id = ++g_hookId; h->target = target; h->mode = mode; h->retval = rv; h->label = label; h->o = so;
  h->tab = so.agg ? new agg::Table() : nullptr; h->owner = DetourOwner(target);
  BuildStub(h); uint8_t* slot = (uint8_t*)h->tramp; void* tramp = nullptr;
  MH_STATUS st = MH_CreateHook((void*)target, h->stub, &tramp);
  if (st != MH_OK) { delete h->tab; delete h; return Fmt("MH_CreateHook failed %d (%s)", st, MH_StatusToString(st)); }
  memcpy(slot, &tramp, 8); h->tramp = tramp;
  st = MH_EnableHook((void*)target);
  if (st != MH_OK) { MH_RemoveHook((void*)target); delete h->tab; delete h; return Fmt("MH_EnableHook failed %d", st); }
  g_hooks.push_back(h);
  LOG("hook %d on 0x%llx %s mode=%d%s", h->id, target, label.c_str(), mode, so.agg ? " agg" : "");
  JsonSetData(jo::Obj().num("id", h->id).hex("target", target).str("label", label).b("agg", so.agg).num("depth", so.depth).done());
  return Fmt("hook %d on 0x%llx %s (%s%s)", h->id, target, label.c_str(), mode ? Fmt("ret 0x%llx", rv).c_str() : "log", so.agg ? Fmt(", agg stack=%d max=%d", so.depth, so.maxLog).c_str() : "");
}
static int RemoveHook(int id) {
  std::lock_guard<std::mutex> l(g_hm); int n = 0;
  for (auto it = g_hooks.begin(); it != g_hooks.end();) {
    if (id < 0 || (*it)->id == id) { MH_DisableHook((void*)(*it)->target); MH_RemoveHook((void*)(*it)->target); n++; it = g_hooks.erase(it);   }
    else ++it;
  }
  return n;
}

struct Bp {
  bool used; uintptr_t addr; int type; int len; volatile LONG64 hits; DWORD lastSec; int secHits;
  agg::Table* tab; SrcOpts o; volatile LONG64 logged; int watchId; volatile LONG64 lastVal; int hookId;
};
static Bp g_bp[4];
static uintptr_t g_bpEver[4];
static PVOID g_veh;

static void ApplyDr(CONTEXT& c) {
  c.Dr7 &= ~0xFFFF00FFull;
  for (int i = 0; i < 4; i++) {
    (&c.Dr0)[i] = g_bp[i].used ? g_bp[i].addr : 0;
    if (!g_bp[i].used) continue;
    int lenBits = g_bp[i].type == 0 ? 0 : g_bp[i].len == 1 ? 0 : g_bp[i].len == 2 ? 1 : g_bp[i].len == 8 ? 2 : 3;
    c.Dr7 |= (1ull << (i * 2)) | ((uint64_t)g_bp[i].type << (16 + i * 4)) | ((uint64_t)lenBits << (18 + i * 4));
  }
}
static int ApplyAllThreads() {
  DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId(); int n = 0;
  HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); THREADENTRY32 te{ sizeof(te) };
  for (BOOL ok = Thread32First(s, &te); ok; ok = Thread32Next(s, &te)) {
    if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
    HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID); if (!t) continue;
    if (SuspendThread(t) != (DWORD)-1) {
      CONTEXT c{}; c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
      if (GetThreadContext(t, &c)) { ApplyDr(c); if (SetThreadContext(t, &c)) n++; }
      ResumeThread(t);
    }
    CloseHandle(t);
  }
  CloseHandle(s); return n;
}
static int RemoveHwHooks(int id) {
  hwh::Slot sl[4]; for (int i = 0; i < 4; i++) { sl[i].used = g_bp[i].used; sl[i].hookId = g_bp[i].hookId; }
  unsigned m = hwh::Pick(sl, 4, id);
  if (!m) return 0;
  for (int i = 0; i < 4; i++) if (m & (1u << i)) g_bp[i].used = false;
  ApplyAllThreads();
  return hwh::Count(m);
}
static std::string RemoveAnyHook(int id) { int n = RemoveHook(id); n += RemoveHwHooks(id); return Fmt("removed %d hook(s)", n); }
static uint64_t ReadLen(uintptr_t a, int len) { uint64_t v = 0; SafeRead(&v, a, len > 8 ? 8 : len); return v; }
static const char* BpKind(int type) { return type == 0 ? "exec" : type == 1 ? "write" : "rw"; }

static LONG CALLBACK Veh(EXCEPTION_POINTERS* e) {
  if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
  CONTEXT* c = e->ContextRecord; uint64_t dr6 = c->Dr6; bool mine = false;
  for (int i = 0; i < 4; i++) {
    if (!(dr6 & (1ull << i))) continue;
    if (!g_bp[i].used || (&c->Dr0)[i] != g_bp[i].addr) {
      if (g_bpEver[i] && (&c->Dr0)[i] == g_bpEver[i]) { mine = true; c->Dr7 &= ~(3ull << (i * 2)); }
      continue;
    }
    mine = true; Bp& b = g_bp[i]; LONG64 n = InterlockedIncrement64(&b.hits);
    if (b.o.fl.any() && !b.o.fl.match(c->Rcx, c->Rdx, c->R8, c->R9)) continue;
    if (b.tab) {
      uint64_t nv = b.type ? ReadLen(b.addr, b.len) : 0;
      uint64_t ov = (uint64_t)InterlockedExchange64(&b.lastVal, (LONG64)nv);
      uint64_t fr[agg::MAXF]; int nf = WalkStack(*c, fr, b.o.depth);
      bool isNew = false; int gi = b.tab->Hit(fr, nf, b.addr, b.addr, ov, nv, GetCurrentThreadId(), H->tickCount, &isNew);
      if (isNew && gi >= 0 && (b.o.maxLog <= 0 || InterlockedIncrement64(&b.logged) <= b.o.maxLog))
        LOG("%s", GroupLine(b.watchId ? Fmt("w%d", b.watchId).c_str() : Fmt("bp%d", i).c_str(), gi, b.tab->g[gi], BpKind(b.type)).c_str());
      continue;
    }
    DWORD sec = GetTickCount() / 1000; if (sec != b.lastSec) { b.lastSec = sec; b.secHits = 0; }
    if (++b.secHits <= 10) {
      uint64_t ret = Rd<uint64_t>(c->Rsp);
      std::string st;
      if (b.o.depth > 0) { uint64_t fr[agg::MAXF]; int nf = WalkStack(*c, fr, b.o.depth); for (int k = 1; k < nf; k++) st += (k > 1 ? " < " : " stack=") + FrameSym(fr[k], true); }
      LOG("bp%d #%lld %s 0x%llx tid=%lu rip=%s rcx=0x%llx rdx=0x%llx r8=0x%llx r9=0x%llx rax=0x%llx rsp=0x%llx [rsp]=%s value=0x%llx%s", i, n,
          BpKind(b.type), b.addr, GetCurrentThreadId(), Sym(c->Rip).c_str(), c->Rcx, c->Rdx, c->R8, c->R9, c->Rax, c->Rsp, Sym(ret).c_str(),
          b.type ? Rd<uint64_t>(b.addr) : 0ull, st.c_str());
    }
  }
  if (!mine) return EXCEPTION_CONTINUE_SEARCH;
  c->Dr6 = 0; c->EFlags |= 0x10000;
  return EXCEPTION_CONTINUE_EXECUTION;
}

static int FreeSlots() { int n = 0; for (auto& b : g_bp) if (!b.used) n++; return n; }

static void ArmSlot(int slot, uintptr_t a, int type, int len, const SrcOpts& so, agg::Table* tab, int watchId) {
  Bp nb{}; nb.used = true; nb.addr = a; nb.type = type; nb.len = len; nb.tab = tab; nb.o = so; nb.watchId = watchId;
  nb.lastVal = (LONG64)(type ? ReadLen(a, len) : 0);
  g_bp[slot] = nb; g_bpEver[slot] = a;
  if (!g_veh) g_veh = AddVectoredExceptionHandler(1, Veh);
}

static std::string SetBp(uintptr_t a, int type, int len, const SrcOpts& so) {
  int slot = -1, want = so.slot;
  if (want >= 0 && want < 4) { if (g_bp[want].used) return Fmt("slot %d is in use (bps)", want); slot = want; }
  else for (int i = 0; i < 4; i++) if (!g_bp[i].used) { slot = i; break; }
  if (slot < 0) return "all 4 debug registers in use (bpc)";
  if (type && (a & (len - 1))) return "address must be aligned to len";
  ArmSlot(slot, a, type, len, so, so.agg ? new agg::Table() : nullptr, 0);
  int n = ApplyAllThreads();
  JsonSetData(jo::Obj().num("slot", slot).str("kind", BpKind(type)).hex("addr", a).num("len", len).num("threads", n).b("agg", so.agg).done());
  return Fmt("bp%d %s 0x%llx len %d set on %d threads (%s)%s", slot, BpKind(type), a, len, n, Sym(a).c_str(), so.agg ? Fmt(" agg stack=%d max=%d", so.depth, so.maxLog).c_str() : "");
}

struct WatchW {
  int id; uintptr_t addr; int size; std::vector<int> slots; ULONGLONG started, deadline; bool active; agg::Table* tab; std::string label; SrcOpts o;
};
static std::mutex g_wwm;
static std::vector<WatchW*> g_ww; static int g_wwId;

static void WatchReport(WatchW* w, int topN, std::vector<std::string>& lines, std::string& json) {
  std::string arr; std::vector<std::string> gl;
  ReportGroups(w->tab, "write", topN, gl, arr);
  ULONGLONG now = GetTickCount64(); double ran = (double)((w->active ? now : std::min(now, w->deadline)) - w->started) / 1000.0;
  lines.push_back(Fmt("watchw %d 0x%llx size %d %s: %d writer groups, %lld writes%s, %.1fs%s", w->id, w->addr, w->size, w->label.c_str(), w->tab->used.load(),
                      (long long)w->tab->total.load(), w->tab->overflow.load() ? " (table overflow)" : "", ran, w->active ? " (still armed)" : " (released)"));
  for (auto& l : gl) lines.push_back(l);
  json = jo::Obj().num("id", w->id).hex("addr", w->addr).num("size", w->size).str("label", w->label).b("active", w->active).num("groups", w->tab->used.load())
             .num("writes", w->tab->total.load()).num("overflow", w->tab->overflow.load()).dbl("seconds", ran).raw("writers", arr).done();
}
static void ReleaseWatch(WatchW* w) {
  if (!w->active) return;
  w->active = false;
  for (int s : w->slots) if (g_bp[s].used && g_bp[s].watchId == w->id) g_bp[s].used = false;
  ApplyAllThreads();
}
static void SummaryToLog(WatchW* w) {
  std::vector<std::string> lines; std::string js; WatchReport(w, 6, lines, js);
  for (auto& l : lines) { size_t nl = l.find('\n'); LOG("%s", (nl == std::string::npos ? l : l.substr(0, nl) + " | " + l.substr(nl + 1)).c_str()); }
}
static void PollWatches() {
  std::lock_guard<std::mutex> l(g_wwm); ULONGLONG now = GetTickCount64();
  for (auto* w : g_ww) if (w->active && now >= w->deadline) { ReleaseWatch(w); SummaryToLog(w); }
}
static HANDLE g_svc; static volatile bool g_svcStop;
static DWORD WINAPI SvcMain(LPVOID) { while (!g_svcStop) { Sleep(50); PollWatches(); } return 0; }

static WatchW* FindWatch(int id) {
  if (id <= 0) return g_ww.empty() ? nullptr : g_ww.back();
  for (auto* w : g_ww) if (w->id == id) return w;
  return nullptr;
}

static void CmdWatchW(const std::vector<std::string>& t, Out& o) {
  std::string err;
  if (t.size() >= 2 && (t[1] == "result" || t[1] == "list" || t[1] == "stop")) {
    std::lock_guard<std::mutex> l(g_wwm);
    int id = t.size() > 2 && isdigit((unsigned char)t[2][0]) ? atoi(t[2].c_str()) : 0; int top = 12;
    for (size_t i = 2; i < t.size(); i++) if (t[i].rfind("top=", 0) == 0) top = atoi(t[i].c_str() + 4);
    if (t[1] == "list") {
      std::vector<std::string> js;
      for (auto* w : g_ww) {
        o.f("watchw %d 0x%llx size %d %s %s groups=%d writes=%lld", w->id, w->addr, w->size, w->label.c_str(), w->active ? "armed" : "released", w->tab->used.load(), (long long)w->tab->total.load());
        js.push_back(jo::Obj().num("id", w->id).hex("addr", w->addr).num("size", w->size).b("active", w->active).num("groups", w->tab->used.load()).num("writes", w->tab->total.load()).done());
      }
      JsonSetData(jo::Obj().raw("watches", jo::Arr(js)).done()); o.f("%zu watches", g_ww.size()); return;
    }
    if (t[1] == "stop") {
      int n = 0; for (auto* w : g_ww) if ((id <= 0 || w->id == id) && w->active) { ReleaseWatch(w); n++; }
      o.f("released %d watch(es)", n); return;
    }
    WatchW* w = FindWatch(id); if (!w) { o.line("no such watch"); return; }
    std::vector<std::string> lines; std::string js; WatchReport(w, top, lines, js);
    for (auto& s : lines) o.line(s);
    JsonSetData(js); return;
  }
  SrcOpts so; std::vector<std::string> rest;
  if (!ParseSrcOpts(t, 1, so, err, &rest)) { o.line("error: " + err); return; }
  if (rest.size() < 2) { o.line("usage: watchw <addr|obj+off> <size 1-64> | watchw <obj> <prop.path>   [secs=30] [stack=12] [max=20] [wait] [rcx=..]   |  watchw result|list|stop [id]"); return; }
  if (so.depth == 0) so.depth = kDefaultDepth;
  if (so.maxLog == 0) so.maxLog = 20;
  if (so.secs == 0) so.secs = 30;
  so.agg = true;
  uint64_t szv = 0; uint64_t a = 0; int size = 0; std::string label = rest[0];
  if (ParseU64(rest[1], szv) && szv >= 1 && szv <= 64) {
    if (!EvalAddr(rest[0], a, err)) { o.line("error: " + err); return; }
    if (InModule(H->base + a)) a += H->base;
    size = (int)szv; label += "+" + rest[1] + "B";
  } else {
    uintptr_t pa = 0; int psz = 0; std::string ty, val;
    if (!UPropAddr(rest[0], rest[1], pa, psz, ty, val, err)) { o.line("error: " + err); return; }
    a = pa; size = std::min(std::max(psz, 1), 64); label = rest[0] + "." + rest[1] + " (" + ty + ")";
  }
  auto chunks = agg::Chunk(a, size);
  { std::lock_guard<std::mutex> l(g_wwm);
    if ((int)chunks.size() > FreeSlots()) { o.f("error: %zu debug registers needed for 0x%llx size %d, %d free (bps / bpc); shrink the size or free slots", chunks.size(), a, size, FreeSlots()); return; }
    WatchW* w = new WatchW{}; w->id = ++g_wwId; w->addr = a; w->size = size; w->started = GetTickCount64(); w->deadline = w->started + (ULONGLONG)so.secs * 1000;
    w->active = true; w->tab = new agg::Table(); w->label = label; w->o = so;
    for (auto& ch : chunks) { int slot = -1; for (int i = 0; i < 4; i++) if (!g_bp[i].used) { slot = i; break; } ArmSlot(slot, ch.first, 1, ch.second, so, w->tab, w->id); w->slots.push_back(slot); }
    g_ww.push_back(w);
    int n = ApplyAllThreads();
    std::string sl; for (int s : w->slots) sl += (sl.empty() ? "bp" : ",bp") + std::to_string(s);
    LOG("watchw %d armed 0x%llx size %d %s on %s for %d s", w->id, a, size, label.c_str(), sl.c_str(), so.secs);
    JsonSetData(jo::Obj().num("id", w->id).hex("addr", a).num("size", size).num("secs", so.secs).num("threads", n).raw("slots", [&] { std::vector<std::string> v; for (int s : w->slots) v.push_back(std::to_string(s)); return jo::Arr(v); }()).done());
    o.f("watchw %d armed 0x%llx size %d (%s) on %s, %d threads, %d s, stack %d. Read: watchw result %d", w->id, a, size, label.c_str(), sl.c_str(), n, so.secs, so.depth, w->id);
    if (!so.wait) return;
  }
  int id = g_wwId;
  ULONGLONG until = GetTickCount64() + (ULONGLONG)(std::min(so.secs, 65) + 3) * 1000;
  for (;;) { { std::lock_guard<std::mutex> l(g_wwm); WatchW* w = FindWatch(id); if (!w || !w->active) break; } if (GetTickCount64() > until) break; Sleep(50); }
  std::lock_guard<std::mutex> l(g_wwm); WatchW* w = FindWatch(id);
  if (w && w->active) { ReleaseWatch(w); }
  if (w) { std::vector<std::string> lines; std::string js; WatchReport(w, 12, lines, js); for (auto& s : lines) o.line(s); JsonSetData(js); }
}

static void CmdAgg(const std::vector<std::string>& t, Out& o) {
  int top = 10; bool clear = false; std::string only;
  for (size_t i = 1; i < t.size(); i++) {
    if (t[i].rfind("top=", 0) == 0) top = atoi(t[i].c_str() + 4);
    else if (t[i] == "top" && i + 1 < t.size()) top = atoi(t[++i].c_str());
    else if (t[i] == "clear") clear = true;
    else only = t[i];
  }
  std::vector<std::string> srcs;
  auto want = [&](const std::string& n) { return only.empty() || only == n; };
  { std::lock_guard<std::mutex> l(g_hm);
    for (auto* h : g_hooks) if (h->tab && want("hook" + std::to_string(h->id))) {
      if (clear) { h->tab->Clear(); h->logged = 0; continue; }
      std::vector<std::string> lines; std::string arr; ReportGroups(h->tab, "hook", top, lines, arr);
      o.f("-- hook%d %s hits=%lld groups=%d filtered=%lld", h->id, h->label.c_str(), (long long)h->hits, h->tab->used.load(), (long long)h->filtered);
      for (auto& s : lines) o.line(s);
      srcs.push_back(jo::Obj().str("src", "hook" + std::to_string(h->id)).str("label", h->label).num("hits", h->hits).num("groups", h->tab->used.load()).num("filtered", h->filtered).raw("top", arr).done());
    } }
  for (int i = 0; i < 4; i++) if (g_bp[i].used && g_bp[i].tab && !g_bp[i].watchId && want("bp" + std::to_string(i))) {
    if (clear) { g_bp[i].tab->Clear(); g_bp[i].logged = 0; continue; }
    std::vector<std::string> lines; std::string arr; ReportGroups(g_bp[i].tab, BpKind(g_bp[i].type), top, lines, arr);
    o.f("-- bp%d %s 0x%llx hits=%lld groups=%d", i, BpKind(g_bp[i].type), g_bp[i].addr, (long long)g_bp[i].hits, g_bp[i].tab->used.load());
    for (auto& s : lines) o.line(s);
    srcs.push_back(jo::Obj().str("src", "bp" + std::to_string(i)).hex("addr", g_bp[i].addr).num("hits", g_bp[i].hits).num("groups", g_bp[i].tab->used.load()).raw("top", arr).done());
  }
  { std::lock_guard<std::mutex> l(g_wwm);
    for (auto* w : g_ww) if (want("w" + std::to_string(w->id))) {
      if (clear) { w->tab->Clear(); continue; }
      std::vector<std::string> lines; std::string js; WatchReport(w, top, lines, js);
      for (auto& s : lines) o.line(s);
      srcs.push_back(js);
    } }
  if (clear) { o.line("cleared"); return; }
  if (srcs.empty()) o.line("no aggregating hook, breakpoint or watch (hook <t> agg | bpw <a> agg | watchw <a> <size>)");
  JsonSetData(jo::Obj().raw("sources", jo::Arr(srcs)).done());
}

void InitHooks() {
  MH_STATUS s = MH_Initialize(); LOG("core MinHook init %d", s);
  g_svcStop = false; g_svc = CreateThread(nullptr, 0, SvcMain, nullptr, 0, nullptr);
}
void ShutdownHooks() {
  g_svcStop = true; if (g_svc) { WaitForSingleObject(g_svc, 3000); CloseHandle(g_svc); g_svc = nullptr; }
  RemoveAnyHook(-1); MH_Uninitialize();
  bool any = false; for (auto& b : g_bp) { any |= b.used; b.used = false; }
  for (auto* w : g_ww) w->active = false;
  if (any) ApplyAllThreads();
  if (g_veh) { RemoveVectoredExceptionHandler(g_veh); g_veh = nullptr; }
  Sleep(30);
}

bool HandleHooks(const std::string& c, const std::vector<std::string>& t, Out& o) {
  std::string err; uint64_t a;
  if (c == "native") {
    if (t.size() < 2) { o.line("usage: native <name>"); return true; }
    std::string lq = Lower(t[1]); int k = 0;
    for (auto& n : g_nat) if (Lower(n.name).find(lq) != std::string::npos && k++ < 40) o.f("%-40s exec 0x%llx (rva 0x%llx) impl %s", n.name.c_str(), H->base + n.exec, n.exec, n.impl ? Fmt("0x%llx (rva 0x%llx)", H->base + n.impl, n.impl).c_str() : "-");
    o.f("%d matches", k); return true;
  }
  if (c == "hook") {
    if (t.size() < 2) { o.line("usage: hook <addr|native> [log|block|ret <v>] [exec] [agg] [stack[=N]] [max=N] [rcx=<expr>] [rdx=..] [r8=..] [r9=..] [hw] [force]"); return true; }
    SrcOpts so; std::vector<std::string> rest;
    if (!ParseSrcOpts(t, 2, so, err, &rest)) { o.line("error: " + err); return true; }
    uintptr_t ex, im; std::string label;
    bool wantExec = std::find(rest.begin(), rest.end(), "exec") != rest.end();
    if (LookupNative(t[1], ex, im)) { a = (im && !wantExec) ? im : ex; label = t[1] + ((im && !wantExec) ? "(impl)" : "(exec)"); }
    else if (EvalAddr(t[1], a, err)) label = Sym(a); else { o.line("cannot resolve " + t[1] + ": " + err); return true; }
    int mode = 0; uint64_t rv = 0;
    if (!rest.empty() && rest[0] == "block") mode = 1;
    if (rest.size() > 1 && rest[0] == "ret") { mode = 1; ParseU64(rest[1], rv); }
    bool force = std::find(rest.begin(), rest.end(), "force") != rest.end();
    auto ni = g_natByRva.find(a - H->base);
    bool isExec = (ni != g_natByRva.end() && g_nat[ni->second].exec == a - H->base) || label.find("(exec)") != std::string::npos;
    if (!isExec) { uint8_t b[4]; if (SafeRead(b, a, 4) && b[0] == 0x48 && b[1] == 0x8B && b[2] == 0x42 && b[3] == 0x20) isExec = true; }
    if (mode && isExec && !force) { o.line("refused: block/ret on a UFunction exec thunk hangs the script VM (it must advance FFrame::Code). Hook the impl, or add force."); return true; }
    if (so.hw) {
      if (mode) { o.line("error: hw supports log only"); return true; }
      if (!so.agg) { so.agg = true; so.depth = so.depth ? so.depth : kDefaultDepth; so.maxLog = so.maxLog ? so.maxLog : kDefaultMaxAgg; }
      std::string r = SetBp(a, 0, 1, so);
      if (r.rfind("bp", 0) == 0) {
        int slot = atoi(r.c_str() + 2), hid = 0;
        { std::lock_guard<std::mutex> hl(g_hm); hid = ++g_hookId; }
        if (slot >= 0 && slot < 4) g_bp[slot].hookId = hid;
        r = "hook(hw) " + r + Fmt(" [hook id %d]", hid);
        JsonSetData(jo::Obj().num("id", hid).hex("target", a).str("mode", "hw").num("slot", slot).done());
      }
      o.line(r); return true;
    }
    bool ours = false; { std::lock_guard<std::mutex> hl(g_hm); for (auto* x : g_hooks) if (x->target == a) ours = true; }
    std::string owner = ours ? "" : DetourOwner(a);
    if (!owner.empty() && !force) {
      std::string who = owner == "?" ? "an unknown module (the detour target is outside every loaded module)" : owner;
      o.line("refused: " + Sym(a) + " is already detoured by " + who + ". Use `hook <t> hw [agg]` (hardware exec breakpoint, no bytes written) or add force.");
      JsonSetData(jo::Obj().str("refused", "detoured").str("owner", owner).hex("target", a).done()); JsonSetOk(false); return true;
    }
    o.line(AddHook(a, mode, rv, label, so)); return true;
  }
  if (c == "unhook") { o.line(RemoveAnyHook(t.size() > 1 && t[1] != "all" ? atoi(t[1].c_str()) : -1)); return true; }
  if (c == "hooks") {
    std::lock_guard<std::mutex> l(g_hm);
    for (auto* h : g_hooks) o.f("%d  0x%llx  %-40s hits=%lld mode=%s%s", h->id, h->target, h->label.c_str(), h->hits, h->mode ? Fmt("ret 0x%llx", h->retval).c_str() : "log",
                                 ((h->tab ? Fmt(" agg groups=%d filtered=%lld", h->tab->used.load(), (long long)h->filtered) : (h->o.fl.any() ? Fmt(" filtered=%lld", (long long)h->filtered) : std::string())) + (h->owner.empty() ? "" : " detoured_by=" + h->owner)).c_str());
    int nhw = 0;
    for (int i = 0; i < 4; i++) if (g_bp[i].used && g_bp[i].hookId) {
      nhw++;
      o.f("%d  0x%llx  %-40s hits=%lld mode=hw slot=%d%s", g_bp[i].hookId, g_bp[i].addr, Sym(g_bp[i].addr).c_str(), (long long)g_bp[i].hits, i,
          g_bp[i].tab ? Fmt(" agg groups=%d", g_bp[i].tab->used.load()).c_str() : "");
    }
    o.f("%zu hooks", g_hooks.size() + nhw); return true;
  }
  if (c == "bpx" || c == "bpw" || c == "bprw") {
    if (t.size() < 2) { o.line("usage: " + c + " <addr> [len] [slot=N] [agg] [stack[=N]] [max=N] [rcx=..]"); return true; }
    SrcOpts so; std::vector<std::string> rest; int len = c == "bpx" ? 1 : 4;
    if (!ParseSrcOpts(t, 2, so, err, &rest)) { o.line("error: " + err); return true; }
    for (auto& s : rest) len = atoi(s.c_str());
    uintptr_t ex, im;
    if (c == "bpx" && LookupNative(t[1], ex, im)) a = im ? im : ex;
    else if (!EvalAddr(t[1], a, err)) { o.line("error: " + err); return true; }
    o.line(SetBp(a, c == "bpx" ? 0 : c == "bpw" ? 1 : 3, len, so)); return true;
  }
  if (c == "bpc") {
    for (int i = 0; i < 4; i++) if (t.size() < 2 || t[1] == "all" || atoi(t[1].c_str()) == i) g_bp[i].used = false;
    o.f("cleared; applied to %d threads", ApplyAllThreads()); return true;
  }
  if (c == "bps") {
    for (int i = 0; i < 4; i++) if (g_bp[i].used) o.f("bp%d %s 0x%llx len %d hits=%lld%s%s", i, BpKind(g_bp[i].type), g_bp[i].addr, g_bp[i].len, g_bp[i].hits,
                                                       g_bp[i].tab ? Fmt(" agg groups=%d", g_bp[i].tab->used.load()).c_str() : "", g_bp[i].watchId ? Fmt(" watchw=%d", g_bp[i].watchId).c_str() : "");
    return true;
  }
  if (c == "watchw") { CmdWatchW(t, o); return true; }
  if (c == "agg") { CmdAgg(t, o); return true; }
  return false;
}
