
#include "core_util.h"
#include "core.h"
#include "home.h"
#include <tlhelp32.h>
#include <psapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <algorithm>
#include <mutex>
#include <deque>
#include <functional>
#include <map>
#include "json_out.h"
#include "stats.h"
#include "sig.h"

HostApi* H;

thread_local int t_objCacheMs = 0;
thread_local int t_jsonSuppress = 0;
static thread_local int t_jsonMode = 0;
static thread_local std::string t_jdata;
static thread_local int t_jok = -1;
void JsonSetData(const std::string& j) { t_jdata = j; }
void JsonSetOk(bool ok) { t_jok = ok ? 1 : 0; }

static int PortNum() {
  char ps[16]; if (GetEnvironmentVariableA("ANOMALY_PORT", ps, sizeof ps)) { int p = atoi(ps); if (p > 0 && p < 65536) return p; }
  return 27060;
}
std::string LogDir() { char d[MAX_PATH]; if (GetEnvironmentVariableA("ANOMALY_LOGDIR", d, sizeof d)) return d; return S2Home(); }
std::string PortLogPath() { return LogDir() + "\\s2dev_" + std::to_string(PortNum()) + ".log"; }
static SRWLOCK g_logLock = SRWLOCK_INIT;
static FILE* g_logF; static long long g_logBytes;
void CoreLogClose() { AcquireSRWLockExclusive(&g_logLock); if (g_logF) { fclose(g_logF); g_logF = nullptr; } ReleaseSRWLockExclusive(&g_logLock); }
void CoreLog(const char* fmt, ...) {
  char msg[4096]; va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof(msg) - 2, fmt, ap); va_end(ap);
  SYSTEMTIME st; GetLocalTime(&st); char line[4200];
  int n = snprintf(line, sizeof line, "[%02d:%02d:%02d.%03d] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
  AcquireSRWLockExclusive(&g_logLock);
  if (!g_logF) { std::string p = PortLogPath(); g_logF = _fsopen(p.c_str(), "ab", _SH_DENYNO); g_logBytes = 0; if (g_logF) { fseek(g_logF, 0, SEEK_END); g_logBytes = ftell(g_logF); } }
  if (g_logF && g_logBytes > (64ll << 20)) { fclose(g_logF); std::string p = PortLogPath(); MoveFileExA(p.c_str(), (p + ".1").c_str(), MOVEFILE_REPLACE_EXISTING); g_logF = _fsopen(p.c_str(), "ab", _SH_DENYNO); g_logBytes = 0; }
  if (g_logF) { fwrite(line, 1, n, g_logF); fflush(g_logF); g_logBytes += n; }
  ReleaseSRWLockExclusive(&g_logLock);
  if (H && H->log) H->log("%s", msg);
}

struct Job { std::function<void()> fn; HANDLE done; };
static std::mutex g_qm;
static std::deque<Job*> g_q;
static std::vector<std::function<void()>> g_tickers;

bool RunOnGame(std::function<void()> fn, DWORD timeoutMs, std::string* err) {
  if (GetCurrentThreadId() == H->gameThreadId) { fn(); return true; }
  uint64_t t0 = H->tickCount;
  Job* j = new Job{ fn, CreateEventA(nullptr, TRUE, FALSE, nullptr) };
  { std::lock_guard<std::mutex> l(g_qm); g_q.push_back(j); }
  if (WaitForSingleObject(j->done, timeoutMs) != WAIT_OBJECT_0) {
    bool removed = false;
    { std::lock_guard<std::mutex> l(g_qm); auto it = std::find(g_q.begin(), g_q.end(), j); if (it != g_q.end()) { g_q.erase(it); removed = true; } }
    if (removed) { CloseHandle(j->done); delete j; if (err) *err = Fmt("game thread timeout (ticks %llu -> %llu)", t0, H->tickCount); return false; }
    WaitForSingleObject(j->done, INFINITE);
  }
  CloseHandle(j->done); delete j; return true;
}
void AddTicker(std::function<void()> fn) { g_tickers.push_back(fn); }

std::string CVarCmd(const std::string& name, const std::string* value);
static void TickAutoexec() {
  static std::vector<std::pair<std::string, std::string>> lines; static bool loaded; static uint64_t next = 300;
  if (H->tickCount < next) return; next = H->tickCount + 600;
  if (!loaded) {
    loaded = true; char p[MAX_PATH]; if (!GetEnvironmentVariableA("ANOMALY_AUTOEXEC", p, sizeof p)) return;
    FILE* f; if (fopen_s(&f, p, "rb")) { H->log("autoexec: cannot open %s", p); return; }
    char b[512]; while (fgets(b, sizeof b, f)) { std::string s = b; s = s.substr(0, s.find('#')); auto t = Tokenize(s); if (t.size() >= 2) lines.push_back({ t[0], t[1] }); }
    fclose(f); H->log("autoexec: %zu cvars from %s", lines.size(), p);
  }
  int changed = 0;
  for (auto& l : lines) {
    std::string cur = CVarCmd(l.first, nullptr); if (cur.empty()) continue;
    std::string v = cur.substr(cur.find(" = ") + 3);
    if (atof(v.c_str()) != atof(l.second.c_str())) { CVarCmd(l.first, &l.second); changed++; }
  }
  if (changed) H->log("autoexec: applied %d cvars", changed);
}

extern "C" __declspec(dllexport) void CoreTick() {
  std::deque<Job*> q;
  { std::lock_guard<std::mutex> l(g_qm); q.swap(g_q); }
  for (Job* j : q) { j->fn(); SetEvent(j->done); }
  TickWatches();
  TickPlugins();
  TickAutoexec();
}

static uintptr_t g_textBeg, g_textEnd, g_imgEnd;
bool InModule(uintptr_t a) { return a >= H->base && a < g_imgEnd; }
bool InText(uintptr_t a) { return a >= g_textBeg && a < g_textEnd; }

static bool ParseExpr(const char*& p, uint64_t& v, std::string& err);
bool LookupName(const std::string& n, uintptr_t& a);
static bool ParseTerm(const char*& p, uint64_t& v, std::string& err) {
  while (*p == ' ') p++;
  if (*p == '[') {
    p++; uint64_t a; if (!ParseExpr(p, a, err)) return false;
    while (*p == ' ') p++; if (*p != ']') { err = "missing ]"; return false; } p++;
    if (!SafeRead(&v, a, 8)) { err = Fmt("bad read at 0x%llx", a); return false; } return true;
  }
  std::string t; while (*p && *p != '+' && *p != '-' && *p != ']' && *p != ' ') t += *p++;
  if (t == "base" || t == "exe") { v = H->base; return true; }
  if (ParseU64(t, v)) return true;
  if (t.find("::") != std::string::npos || t.rfind("sym:", 0) == 0) { uintptr_t x; if (LookupName(t.rfind("sym:", 0) == 0 ? t.substr(4) : t, x)) { v = x; return true; } }
  uintptr_t o = ResolveObjCached(t); if (o) { v = o; return true; }
  err = "bad term '" + t + "'"; return false;
}
static bool ParseExpr(const char*& p, uint64_t& v, std::string& err) {
  if (!ParseTerm(p, v, err)) return false;
  for (;;) {
    while (*p == ' ') p++;
    if (*p != '+' && *p != '-') return true;
    char op = *p++; uint64_t w; if (!ParseTerm(p, w, err)) return false; v = op == '+' ? v + w : v - w;
  }
}
bool EvalAddr(const std::string& s, uint64_t& v, std::string& err) {
  const char* p = s.c_str(); if (!ParseExpr(p, v, err)) return false;
  while (*p == ' ') p++; if (*p) { err = "trailing junk"; return false; } return true;
}

static void HexDump(Out& o, uintptr_t a, size_t n) {
  for (size_t off = 0; off < n; off += 16) {
    uint8_t b[16]; size_t k = std::min<size_t>(16, n - off);
    if (!SafeRead(b, a + off, k)) { o.f("%016llx  <unreadable>", a + off); return; }
    std::string hex, asc;
    for (size_t i = 0; i < 16; i++) { if (i < k) { hex += Fmt("%02x ", b[i]); asc += (b[i] >= 32 && b[i] < 127) ? (char)b[i] : '.'; } else hex += "   "; }
    o.f("%016llx  %s %s", a + off, hex.c_str(), asc.c_str());
  }
}

static std::vector<uintptr_t> Aob(const std::string& pat, uintptr_t beg, uintptr_t end, size_t maxHits) {
  std::vector<int> bytes; auto t = Tokenize(pat);
  for (auto& x : t) bytes.push_back((x == "?" || x == "??") ? -1 : (int)strtoul(x.c_str(), nullptr, 16));
  std::vector<uintptr_t> r; if (bytes.empty()) return r;
  const uint8_t* p = (const uint8_t*)beg; size_t n = end - beg - bytes.size();
  for (size_t i = 0; i < n && r.size() < maxHits; i++) {
    size_t j = 0; for (; j < bytes.size(); j++) if (bytes[j] >= 0 && p[i + j] != bytes[j]) break;
    if (j == bytes.size()) r.push_back(beg + i);
  }
  return r;
}

std::string ModOf(uintptr_t a) {
  HMODULE m; char nm[MAX_PATH];
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m) && GetModuleFileNameA(m, nm, MAX_PATH)) {
    const char* s = strrchr(nm, '\\'); return Fmt("%s+0x%llx", s ? s + 1 : nm, a - (uintptr_t)m);
  }
  return "?";
}
struct NameTab { std::vector<std::pair<uint32_t, std::string>> v; std::map<std::string, uint32_t> byLower; };
static NameTab* volatile g_names;
static int LoadNames() {
  FILE* f; if (fopen_s(&f, S2Env("ANOMALY_NAMES", S2Path("names\\names.tsv")).c_str(), "rb")) return -1;
  NameTab* t = new NameTab; char line[1024];
  while (fgets(line, sizeof(line), f)) {
    char* p1 = strchr(line, '\t'); if (!p1) continue; char* p2 = strchr(p1 + 1, '\t'); if (!p2) continue; char* p3 = strchr(p2 + 1, '\t');
    char* e; uint32_t r = (uint32_t)strtoul(line, &e, 16); if (e != p1 || !r) continue;
    std::string nm(p2 + 1, p3 ? p3 : p2 + 1 + strcspn(p2 + 1, "\r\n"));
    t->v.push_back({ r, nm }); t->byLower.emplace(Lower(nm), r);
  }
  fclose(f);
  std::sort(t->v.begin(), t->v.end(), [](const std::pair<uint32_t, std::string>& a, const std::pair<uint32_t, std::string>& b) { return a.first < b.first; });
  g_names = t; return (int)t->v.size();
}
static const std::string* NameAt(uint32_t rva) {
  NameTab* t = g_names; if (!t) return nullptr;
  auto it = std::lower_bound(t->v.begin(), t->v.end(), rva, [](const std::pair<uint32_t, std::string>& a, uint32_t r) { return a.first < r; });
  return (it != t->v.end() && it->first == rva) ? &it->second : nullptr;
}
static uint32_t FuncStart(uintptr_t a) {
  DWORD64 ib = 0; auto rf = RtlLookupFunctionEntry(a, &ib, nullptr); if (!rf || ib != H->base) return 0;
  for (int g = 0; g < 8; g++) {
    uintptr_t ui = H->base + rf->UnwindData; uint8_t vf = Rd<uint8_t>(ui), cnt = Rd<uint8_t>(ui + 2);
    if (!((vf >> 3) & 4)) break;
    rf = (PRUNTIME_FUNCTION)(ui + 4 + ((cnt + 1) & ~1) * 2);
  }
  return rf->BeginAddress;
}
bool LookupName(const std::string& n, uintptr_t& a) {
  NameTab* t = g_names; if (!t) return false; auto it = t->byLower.find(Lower(n)); if (it == t->byLower.end()) return false;
  a = H->base + it->second; return true;
}
std::string Sym(uintptr_t a) {
  if (InModule(a)) {
    uint32_t r = (uint32_t)(a - H->base);
    if (auto n = NameAt(r)) return Fmt("exe+0x%x %s", r, n->c_str());
    if (InText(a)) { uint32_t fs = FuncStart(a); if (fs) { auto n = NameAt(fs); return Fmt("exe+0x%x %s+0x%x", r, n ? n->c_str() : Fmt("sub_%llx", (unsigned long long)fs + 0x140000000ULL).c_str(), r - fs); } }
    if (!InText(a)) if (NameTab* t = g_names) {
      auto it = std::upper_bound(t->v.begin(), t->v.end(), r, [](uint32_t x, const std::pair<uint32_t, std::string>& e) { return x < e.first; });
      if (it != t->v.begin() && r - (--it)->first < 0x2000) return Fmt("exe+0x%x %s+0x%x", r, it->second.c_str(), r - it->first);
    }
    std::string n = NativeNameAt(a); return Fmt("exe+0x%x%s%s", r, n.empty() ? "" : " ", n.c_str());
  }
  return ModOf(a);
}
std::string FrameSym(uint64_t a, bool isReturn) {
  uintptr_t p = (uintptr_t)(isReturn && a ? a - 1 : a);
  if (InModule(p)) {
    uint32_t fs = InText(p) ? FuncStart(p) : 0;
    if (fs) {
      auto n = NameAt(fs); std::string nm = n ? *n : Fmt("FUN_%llx", (unsigned long long)fs + 0x140000000ULL);
      uint32_t off = (uint32_t)(a - (H->base + fs));
      return off ? Fmt("%s+0x%x", nm.c_str(), off) : nm;
    }
    return Fmt("exe+0x%llx", (unsigned long long)(a - H->base));
  }
  return ModOf((uintptr_t)a);
}
uintptr_t ResolveObjCached(const std::string& s) {
  if (t_objCacheMs <= 0) return ResolveObj(s);
  static thread_local std::map<std::string, std::pair<uintptr_t, ULONGLONG>> cache;
  ULONGLONG now = GetTickCount64(); auto it = cache.find(s);
  if (it != cache.end() && now - it->second.second < (ULONGLONG)t_objCacheMs) return it->second.first;
  uintptr_t o = ResolveObj(s); cache[s] = { o, now }; return o;
}
#include "minhook/src/hde/hde64.h"
static std::string Callees(const std::string& in, const std::string& out) {
  FILE* fi; if (fopen_s(&fi, in.c_str(), "rb")) return "cannot open " + in;
  FILE* fo; if (fopen_s(&fo, out.c_str(), "wb")) { fclose(fi); return "cannot open " + out; }
  char line[256]; int n = 0;
  while (fgets(line, sizeof(line), fi)) {
    uint32_t r = (uint32_t)strtoul(line, nullptr, 16); if (!r) continue;
    uintptr_t a = H->base + r; DWORD64 ib = 0; auto rf = RtlLookupFunctionEntry(a, &ib, nullptr);
    uintptr_t beg = a, end = a + 0x200; if (rf && ib == H->base) { beg = H->base + rf->BeginAddress; end = H->base + rf->EndAddress; }
    if (end - beg > 0x4000) end = beg + 0x4000;
    std::string s;
    for (uintptr_t p = beg; p < end;) {
      uint8_t b[16]; if (!SafeRead(b, p, 16)) break; hde64s hs; unsigned len = hde64_disasm(b, &hs); if (!len || (hs.flags & F_ERROR)) break;
      if ((hs.opcode == 0xE8 || hs.opcode == 0xE9) && len == 5) {
        uintptr_t tg = p + len + (int32_t)hs.imm.imm32;
        if (InText(tg) && (hs.opcode == 0xE8 || tg < beg || tg >= end)) s += Fmt("%s%c:%llx", s.empty() ? "" : ",", hs.opcode == 0xE8 ? 'C' : 'J', (unsigned long long)(tg - H->base));
      }
      p += len;
    }
    fprintf(fo, "%x\t%x\t%s\n", r, (unsigned)(beg - H->base), s.c_str()); n++;
  }
  fclose(fi); fclose(fo); return Fmt("%d functions -> %s", n, out.c_str());
}

static void CmdLogTail(Out& o, int n) {
  FILE* f; std::string lp = PortLogPath(); if (fopen_s(&f, lp.c_str(), "rb") && fopen_s(&f, (LogDir() + "\\s2dev.log").c_str(), "rb")) { o.line("cannot open log"); return; }
  _fseeki64(f, 0, SEEK_END); long long sz = _ftelli64(f); long long st = std::max(0LL, sz - 64 * 1024);
  _fseeki64(f, st, SEEK_SET); std::string d((size_t)(sz - st), 0); fread(&d[0], 1, d.size(), f); fclose(f);
  std::vector<std::string> lines; size_t s = 0, e;
  while ((e = d.find('\n', s)) != std::string::npos) { lines.push_back(d.substr(s, e - s)); s = e + 1; }
  for (size_t i = lines.size() > (size_t)n ? lines.size() - n : 0; i < lines.size(); i++) o.line(lines[i]);
}

static const char* HELP =
  "loader: ping | reload | unload\n"
  "mem:    rd <addr> [n] | wr <addr> <hexbytes> | ptr <expr e.g. [[pawn+0x650]+0x660]+0x38> | aob <pattern> | rva <addr> | va <rva> | mods | sym <addr> | addr <name|substr> | names (reload names.tsv)\n"
  "export: dumpfuncs <file> | dumpvtables <file> | dumpsdk <file> | dumpfieldvt <file> | callees <rvalist> <out>\n"
  "uobj:   find <substr|class:Name|isa:Name> [max] [near=<obj|x,y,z>] [r=cm] [loc] [new=N] [since=idx] [hidden=0|1] [nodef] | npcs [near=..] [r=..] [class=..] [uid=..] [new=N] [max=N] [all] | uid <obj> | comps <obj> [filter=..] [flat] | pose <actor> [bones=hands|arms|legs|all|a,b] [ref=<actor>] [comps] [space=world|comp|parent] | quest list|state <sid>|tap on|off|clear|show|end <node>|start <node>|layout | tarray|tset|tmap <obj> <Prop.Path> [max=N] [key=..] | tset raw <addr> <elemsize> [max] | layoutcheck <Class> [obj] | obj <addr|name|pawn|pc> | get <obj> <prop.path> | set <obj> <prop.path> <value>\n"
  "        call <obj> <Func> [args...] | funcs <class|obj> [filter] | classes [filter] | props <class|obj>\n"
  "        watchprop <obj> <prop.path> | unwatch [id] | con <console command>\n"
  "native: native <name>\n"
  "hooks:  hook <addr|native> [log|block|ret <v>] [agg] [hw] [force] | unhook <id|all> | hooks\n"
  "hwbp:   bpx <addr> | bpw <addr> [len 1|2|4|8] | bprw <addr> [len] | bpc <slot|all> | bps\n"
  "probe:  sample <cols> <hz> <secs> | memdiff <addr|obj> <len|auto> <secs> [hz=20] [wait] | memdiff result|list|stop [id]\n"
  "misc:   log [n] | stats [clear] | help\n"
  "<addr> may be an expression: base+0x50C40C, [pawn+0x338]+0x10, 0x7ff..., pawn, pc, <object name>";

typedef uint64_t(__fastcall* X4)(uint64_t, uint64_t, uint64_t, uint64_t);
static int SafeXCall(uintptr_t f, const uint64_t* a, uint64_t* r) { __try { *r = ((X4)f)(a[0], a[1], a[2], a[3]); return 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return (int)GetExceptionCode(); } }
static uintptr_t GConsoleManagerAddr() { static uintptr_t a = SigFind(H->base, SIG_GCONSOLEMANAGER, SIG_GCONSOLEMANAGER_RIP); return a; }
struct FStrRaw { wchar_t* d; int n, m; };
static uintptr_t FindCVar(const std::string& name) {
  uintptr_t cm = GConsoleManagerAddr() ? RdP(GConsoleManagerAddr()) : 0; if (!cm) return 0;
  std::wstring w = Widen(name); uint64_t ar[4] = { cm, (uint64_t)w.c_str(), 1, 0 }, r = 0;
  return SafeXCall(RdP(RdP(cm) + 0xb8), ar, &r) ? 0 : (uintptr_t)r;
}
static std::string CVarGet(uintptr_t cv) {
  FStrRaw s{}; uint64_t ar[4] = { cv, (uint64_t)&s, 0, 0 }, r;
  if (SafeXCall(RdP(RdP(cv) + 0xc0), ar, &r) || !s.d || s.n <= 0) return "?";
  return Narrow(s.d, s.n - 1);
}
std::string CVarCmd(const std::string& name, const std::string* value) {
  uintptr_t cv = FindCVar(name); if (!cv) return "";
  std::string before = CVarGet(cv);
  if (!value) return name + " = " + before;
  std::wstring w = Widen(*value); std::string err; int code = 0;
  if (!RunOnGame([&]() { uint64_t ar[4] = { cv, (uint64_t)w.c_str(), 0x0E000000, 0 }, r; code = SafeXCall(RdP(RdP(cv) + 0x98), ar, &r); }, 5000, &err)) return err;
  if (code) return Fmt("exception 0x%08x in IConsoleVariable::Set", code);
  return name + ": " + before + " -> " + CVarGet(cv);
}
static void CmdMods(Out& o) {
  HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId()); MODULEENTRY32 me{ sizeof(me) };
  for (BOOL ok = Module32First(s, &me); ok; ok = Module32Next(s, &me)) o.f("%016llx %10lx %s", (uint64_t)me.modBaseAddr, me.modBaseSize, me.szModule);
  CloseHandle(s);
}

static std::string BuildInfo() {
  static std::string s;
  if (!s.empty()) return s;
  HMODULE me = nullptr; char p[MAX_PATH] = {};
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&BuildInfo, &me);
  std::string cm = me && GetModuleFileNameA(me, p, MAX_PATH) ? FileMd5(p) : "";
  std::string lm; HMODULE ld = GetModuleHandleA("S2Dev.dll"); if (ld && GetModuleFileNameA(ld, p, MAX_PATH)) lm = FileMd5(p);
  s = Fmt(" md5=%s loader_md5=%s port=%d", cm.empty() ? "?" : cm.c_str(), lm.empty() ? "?" : lm.c_str(), PortNum());
  return s;
}

static void HandleText(const char* line, EmitFn emit, void* ctx) {
  Out o{ emit, ctx };
  std::string L = line; auto t = Tokenize(L); if (t.empty()) return;
  std::string c = Lower(t[0]); std::string err; uint64_t a;
  auto rest = [&](size_t i) { size_t p = 0; for (size_t k = 0; k < i; k++) { while (p < L.size() && L[p] == ' ') p++; if (p < L.size() && L[p] == '"') { p = L.find('"', p + 1); p = p == std::string::npos ? L.size() : p + 1; } else while (p < L.size() && L[p] != ' ') p++; } while (p < L.size() && L[p] == ' ') p++; return L.substr(std::min(p, L.size())); };
  if (c == "help") o.line(HELP);
  else if (c == "ver") o.f("S2DevCore built %s %s ticks=%llu%s", __DATE__, __TIME__, H->tickCount, BuildInfo().c_str());
  else if (c == "ping") o.f("pong pid=%lu base=0x%llx ticks=%llu core=loaded", GetCurrentProcessId(), (unsigned long long)H->base, H->tickCount);
  else if (c == "mark") {
    std::string txt = rest(1); if (txt.empty()) txt = "mark";
    CoreLog("==== MARK %s ====", txt.c_str());
    WIN32_FILE_ATTRIBUTE_DATA fa{}; GetFileAttributesExA(PortLogPath().c_str(), GetFileExInfoStandard, &fa);
    o.f("mark \"%s\" log=%s offset=%llu", txt.c_str(), PortLogPath().c_str(), ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow);
  }
  else if (c == "log") CmdLogTail(o, t.size() > 1 ? atoi(t[1].c_str()) : 30);
  else if (c == "stats") {
    if (t.size() > 1 && Lower(t[1]) == "clear") { cst::Clear(); o.line("stats cleared"); return; }
    std::vector<std::string> js; o.line("cmd count total_ms max_ms");
    for (auto& s : cst::Rows()) { o.f("%s %lld %lld %lld", s.cmd.c_str(), s.count, s.total, s.max); js.push_back(jo::Obj().str("cmd", s.cmd).num("count", s.count).num("total_ms", s.total).num("max_ms", s.max).done()); }
    JsonSetData(jo::Obj().raw("stats", jo::Arr(js)).done());
  }
  else if (c == "mods") CmdMods(o);
  else if (c == "rd" || c == "ptr" || c == "sym" || c == "rva") {
    if (t.size() < 2 || !EvalAddr(c == "ptr" ? rest(1) : t[1], a, err)) { o.line("error: " + err); return; }
    if (c != "ptr" && a < g_imgEnd - H->base) a += H->base;
    if (c == "rd") { uint64_t n = 64; if (t.size() > 2) ParseU64(t[2], n); HexDump(o, a, (size_t)std::min<uint64_t>(n, 4096)); }
    else if (c == "rva") o.f("rva 0x%llx (%s)", a - H->base, Sym(a).c_str());
    else { uint64_t v = Rd<uint64_t>(a, 0); o.f("0x%llx  [%s]  -> qword 0x%llx float %g double %g int %d", a, Sym(a).c_str(), v, Rd<float>(a), Rd<double>(a), Rd<int>(a)); std::string d = DescribeObj(a); if (!d.empty()) o.line("  obj: " + d); }
  }
  else if (c == "addr") {
    if (t.size() < 2) { o.line("usage: addr <name|substring>"); return; }
    uintptr_t x; if (LookupName(t[1], x)) { o.f("0x%llx  rva 0x%llx  %s", x, x - H->base, Sym(x).c_str()); return; }
    NameTab* tb = g_names; if (!tb) { o.line("names.tsv not loaded"); return; }
    std::string lq = Lower(t[1]); int hits = 0;
    for (auto& e : tb->v) if (Lower(e.second).find(lq) != std::string::npos && hits++ < 40) o.f("0x%llx  rva 0x%x  %s", H->base + e.first, e.first, e.second.c_str());
    o.f("%d matches", hits);
  }
  else if (c == "names") o.f("names.tsv: %d entries loaded", LoadNames());
  else if (c == "callees") { if (t.size() < 3) { o.line("usage: callees <in rva list> <out>"); return; } o.line(Callees(t[1], t[2])); }
  else if (c == "va") { if (t.size() < 2 || !ParseU64(t[1], a)) { o.line("usage: va <rva>"); return; } o.f("0x%llx", H->base + a); }
  else if (c == "wr") {
    if (t.size() < 3 || !EvalAddr(t[1], a, err)) { o.line("usage: wr <addr> <hex>"); return; }
    std::string hx = rest(2); hx.erase(std::remove(hx.begin(), hx.end(), ' '), hx.end());
    std::vector<uint8_t> b; for (size_t i = 0; i + 1 < hx.size(); i += 2) b.push_back((uint8_t)strtoul(hx.substr(i, 2).c_str(), nullptr, 16));
    bool ok = SafeWrite(a, b.data(), b.size()); o.f("%s %zu bytes at 0x%llx", ok ? "wrote" : "FAILED", b.size(), a); HexDump(o, a, b.size());
  }
  else if (c == "xcall") {
    if (t.size() < 2 || !EvalAddr(t[1], a, err)) { o.line("usage: xcall <addr|Name> [rcx rdx r8 r9] (" + err + ")"); return; }
    uint64_t ar[4] = {}; for (size_t i = 2; i < t.size() && i < 6; i++) if (!EvalAddr(t[i], ar[i - 2], err)) { o.line("arg: " + err); return; }
    uint64_t r = 0; int code = 0;
    if (!RunOnGame([&]() { code = SafeXCall((uintptr_t)a, ar, &r); }, 5000, &err)) { o.line(err); return; }
    if (code) o.f("exception 0x%08x calling %s", code, Sym(a).c_str()); else o.f("rax=0x%llx (al=%u eax=%d) %s", r, (unsigned)(r & 0xff), (int)r, Sym(a).c_str());
  }
  else if (c == "cvar") {
    if (t.size() < 2) { o.line("usage: cvar <name> [value]"); return; }
    std::string v = t.size() > 2 ? rest(2) : ""; std::string r = CVarCmd(t[1], t.size() > 2 ? &v : nullptr);
    o.line(r.empty() ? "no console variable " + t[1] : r);
  }
  else if (c == "cvarobj") {
    if (t.size() < 2) { o.line("usage: cvarobj <name>"); return; }
    uintptr_t cm = GConsoleManagerAddr() ? RdP(GConsoleManagerAddr()) : 0; if (!cm) { o.line("no console manager"); return; }
    std::wstring w = Widen(t[1]); uint64_t ar[4] = { cm, (uint64_t)w.c_str(), 0, 0 }, r = 0;
    int code = SafeXCall(RdP(RdP(cm) + 0xb8), ar, &r);
    if (code || !r) { o.f("not found (code 0x%x)", code); return; }
    uintptr_t vt = RdP(r); o.f("cvar %s = 0x%llx vtable %s", t[1].c_str(), r, Sym(vt).c_str());
    for (int i = 0; i < 32; i++) o.f("  slot %2d +0x%02x %s", i, i * 8, Sym(RdP(vt + i * 8)).c_str());
  }
  else if (c == "aob") {
    auto h = Aob(rest(1), g_textBeg, g_textEnd, 32);
    for (auto x : h) o.f("0x%llx  %s", x, Sym(x).c_str()); o.f("%zu hits (max 32)", h.size());
  }
  else if (!HandleUObj(c, t, L, o) && !HandleHooks(c, t, o) && !HandleProbe(c, t, L, o) && !HandlePlugins(c, t, o)) o.line("unknown command (help)");
}

extern "C" __declspec(dllexport) void CoreHandle(const char* line, EmitFn emit, void* ctx) {
  std::string L = line; auto t = Tokenize(L);
  bool oneShot = false;
  if (!t.empty() && Lower(t[0]) == "json" && !t_jsonSuppress) {
    std::string sub = t.size() > 1 ? Lower(t[1]) : "status";
    if (sub == "on") { t_jsonMode = 1; L = "ver"; }
    else if (sub == "off" || sub == "status") { if (sub == "off") t_jsonMode = 0; Out o{ emit, ctx }; o.f("json mode %s", t_jsonMode ? "on" : "off"); return; }
    else { size_t p = L.find(' '); L = L.substr(p == std::string::npos ? L.size() : p + 1); oneShot = true; }
  }
  ULONGLONG t0 = GetTickCount64();
  auto note = [&]() {
    long long ms = (long long)(GetTickCount64() - t0); auto k = Tokenize(L);
    cst::Add(k.empty() ? "" : Lower(k[0]), ms);
    if (ms >= cst::SlowMs()) CoreLog("slow %lld ms: %.80s", ms, L.c_str());
  };
  if (t_jsonSuppress || !(oneShot || t_jsonMode)) { HandleText(L.c_str(), emit, ctx); note(); return; }
  t_jdata.clear(); t_jok = -1;
  std::string buf;
  HandleText(L.c_str(), [](void* c, const char* s) { *(std::string*)c += s; }, &buf);
  note();
  auto lines = jo::SplitLines(buf);
  auto tk = Tokenize(L); std::string cmd = tk.empty() ? "" : Lower(tk[0]);
  bool ok = t_jok >= 0 ? t_jok == 1 : !(!lines.empty() && jo::LooksLikeError(lines[0]));
  std::string data = !t_jdata.empty() ? t_jdata : (ok ? jo::Structure(cmd, lines) : "");
  std::string out = jo::Envelope(cmd, ok, lines, data, PortNum(), (long long)(GetTickCount64() - t0)) + "\n";
  emit(ctx, out.c_str());
}

extern "C" __declspec(dllexport) int CoreInit(HostApi* api) {
  H = api;
  auto nt = (IMAGE_NT_HEADERS*)(H->base + ((IMAGE_DOS_HEADER*)H->base)->e_lfanew);
  g_imgEnd = H->base + nt->OptionalHeader.SizeOfImage;
  auto sec = IMAGE_FIRST_SECTION(nt);
  for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) if (!memcmp(sec[i].Name, ".text", 5)) { g_textBeg = H->base + sec[i].VirtualAddress; g_textEnd = g_textBeg + sec[i].Misc.VirtualSize; }
  LoadNatives();
  LOG("names.tsv: %d entries", LoadNames());
  InitUObj();
  InitHooks();
  InitPlugins();
  InitProbe();
  LOG("core init done (built %s %s)", __DATE__, __TIME__);
  return 1;
}

extern "C" __declspec(dllexport) void CoreShutdown() {
  ShutdownProbe();
  ShutdownPlugins();
  ShutdownHooks();
  std::deque<Job*> q; { std::lock_guard<std::mutex> l(g_qm); q.swap(g_q); }
  for (Job* j : q) SetEvent(j->done);
  LOG("core shutdown");
  CoreLogClose();
}


