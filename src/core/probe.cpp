#include "core.h"
#include "json_out.h"
#include "sampler.h"
#include "memdiff.h"
#include <memory>
#include <algorithm>
#include <mutex>

static std::mutex g_sm;
static std::vector<std::shared_ptr<smp::Run>> g_runs;
static int g_runId;

static bool EvalColumn(const smp::Col& c, std::string& out) {
  if (c.isProp) {
    uintptr_t a; int sz; std::string ty, val, err;
    if (!UPropAddr(c.obj, c.path, a, sz, ty, val, err)) return false;
    out = val; return true;
  }
  uint64_t a; std::string err;
  if (!EvalAddr(c.expr, a, err)) return false;
  if (InModule(H->base + a)) a += H->base;
  uint8_t b[8] = {}; if (!SafeRead(b, a, smp::TypeSize(c.type))) return false;
  out = smp::FormatValue(c.type, b); return true;
}

static std::string TailLines(const std::string& file, int n) {
  FILE* f; if (fopen_s(&f, file.c_str(), "rb")) return "";
  _fseeki64(f, 0, SEEK_END); long long sz = _ftelli64(f); long long st = sz > 65536 ? sz - 65536 : 0;
  _fseeki64(f, st, SEEK_SET); std::string d((size_t)(sz - st), 0); size_t k = fread(&d[0], 1, d.size(), f); d.resize(k); fclose(f);
  auto lines = jo::SplitLines(d); std::string r;
  for (size_t i = lines.size() > (size_t)n ? lines.size() - n : 0; i < lines.size(); i++) r += lines[i] + "\n";
  return r;
}

static std::shared_ptr<smp::Run> FindRun(const std::string& id) {
  if (id.empty()) return g_runs.empty() ? nullptr : g_runs.back();
  for (auto& r : g_runs) if (r->id == id) return r;
  return nullptr;
}

static void CmdSample(const std::vector<std::string>& t, Out& o) {
  std::string sub = t.size() > 1 ? Lower(t[1]) : "";
  if (sub == "status" || sub == "list") {
    std::lock_guard<std::mutex> l(g_sm); std::vector<std::string> js;
    for (auto& r : g_runs) {
      o.f("%s %s rows=%ld hz=%d secs=%d %s", r->id.c_str(), r->done ? "done" : "running", r->rows.load(), r->hz, r->durMs / 1000, r->file.c_str());
      js.push_back(jo::Obj().str("id", r->id).b("done", r->done).num("rows", r->rows).num("hz", r->hz).str("file", r->file).done());
    }
    JsonSetData(jo::Obj().raw("samplers", jo::Arr(js)).done()); o.f("%zu samplers", g_runs.size()); return;
  }
  if (sub == "stop") {
    std::lock_guard<std::mutex> l(g_sm); int n = 0;
    for (auto& r : g_runs) if (!r->done && (t.size() < 3 || t[2] == "all" || t[2] == r->id)) { r->stop = true; n++; }
    o.f("stopping %d sampler(s)", n); return;
  }
  if (sub == "show") {
    std::lock_guard<std::mutex> l(g_sm); std::shared_ptr<smp::Run> r = FindRun(t.size() > 2 && t[2][0] == 's' ? t[2] : "");
    if (!r) { o.line("no such sampler"); return; }
    int n = 20; for (size_t i = 2; i < t.size(); i++) if (t[i].rfind("n=", 0) == 0) n = atoi(t[i].c_str() + 2);
    std::string tail = TailLines(r->file, n); o.line(tail.empty() ? "(no rows yet)" : tail.substr(0, tail.size() - 1));
    JsonSetData(jo::Obj().str("id", r->id).str("file", r->file).num("rows", r->rows).b("done", r->done).done()); return;
  }
  if (t.size() < 4) { o.line("usage: sample <cols> <hz> <secs> [file=PATH] [name=N] [wait]   cols = name=<addr expr>:<f32|f64|i8..u64|hex|bool> or name=@obj|Prop.Path, comma separated   |  sample status|stop|show [id] [n=20]"); return; }
  std::vector<smp::Col> cols; std::string err;
  if (!smp::ParseCols(t[1], cols, err)) { o.line("error: " + err); return; }
  int hz = atoi(t[2].c_str()), secs = atoi(t[3].c_str());
  if (hz < 1 || hz > 200 || secs < 1 || secs > 3600) { o.line("error: hz 1-200, secs 1-3600"); return; }
  std::string file, name = "run"; bool wait = false;
  for (size_t i = 4; i < t.size(); i++) { if (t[i].rfind("file=", 0) == 0) file = t[i].substr(5); else if (t[i].rfind("name=", 0) == 0) name = t[i].substr(5); else if (t[i] == "wait") wait = true; }
  std::unique_lock<std::mutex> l(g_sm);
  int running = 0; for (auto& r : g_runs) if (!r->done) running++;
  if (running >= 4) { o.line("error: 4 samplers already running (sample stop)"); return; }
  auto r = std::make_shared<smp::Run>();
  r->id = "s" + std::to_string(++g_runId); r->cols = cols; r->hz = hz; r->durMs = secs * 1000;
  if (file.empty()) {
    SYSTEMTIME s; GetLocalTime(&s); std::string dir = LogDir() + "\\samples"; CreateDirectoryA(dir.c_str(), nullptr);
    std::string nm; for (char ch : name) nm += (isalnum((unsigned char)ch) || ch == '_' || ch == '-') ? ch : '_';
    file = Fmt("%s\\%s_%04d%02d%02d_%02d%02d%02d_%s.tsv", dir.c_str(), nm.c_str(), s.wYear, s.wMonth, s.wDay, s.wHour, s.wMinute, s.wSecond, r->id.c_str());
  }
  r->file = file;
  r->eval = [](const smp::Col& c, std::string& out) { return EvalColumn(c, out); };
  r->tick = []() { return (uint64_t)H->tickCount; };
  std::shared_ptr<smp::Run> raw = r;
  raw->th = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD { t_objCacheMs = 500; return smp::Thunk(p); }, raw.get(), 0, nullptr);
  g_runs.push_back(std::move(r));
  JsonSetData(jo::Obj().str("id", raw->id).str("file", file).num("hz", hz).num("secs", secs).num("columns", (long long)cols.size()).done());
  o.f("sample %s started: %zu columns, %d Hz, %d s -> %s", raw->id.c_str(), cols.size(), hz, secs, file.c_str());
  if (!wait) return;
  std::string id = raw->id;
  l.unlock();
  ULONGLONG until = GetTickCount64() + (ULONGLONG)(std::min(secs, 65) + 5) * 1000;
  while (!raw->done && GetTickCount64() < until) Sleep(50);
  l.lock();
  o.f("sample %s %s: %ld rows", id.c_str(), raw->done ? "done" : "still running", raw->rows.load());
  std::string tail = TailLines(file, 6); if (!tail.empty()) o.line(tail.substr(0, tail.size() - 1));
  JsonSetData(jo::Obj().str("id", id).str("file", file).num("rows", raw->rows).b("done", raw->done).done());
}

struct MemRun {
  std::string id, name, err; uintptr_t addr = 0; int len = 0, hz = 20, durMs = 0; bool isObj = false;
  std::atomic<bool> stop{ false }, done{ false }; std::atomic<long> samples{ 0 };
  std::vector<mdf::Cell> cells; std::mutex m; HANDLE th = nullptr;
};
static std::mutex g_mm;
static std::vector<std::shared_ptr<MemRun>> g_mruns;
static int g_mrunId;

static DWORD WINAPI MemLoop(LPVOID p) {
  MemRun* r = (MemRun*)p; timeBeginPeriod(1);
  std::vector<uint8_t> prev(r->len), cur(r->len);
  if (!SafeRead(prev.data(), r->addr, r->len)) { r->err = "unreadable"; r->done = true; timeEndPeriod(1); return 0; }
  LARGE_INTEGER fq, t0, now; QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&t0);
  double period = 1000.0 / r->hz, next = period;
  while (!r->stop) {
    QueryPerformanceCounter(&now);
    double ms = (double)(now.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart;
    if (ms > r->durMs) break;
    if (ms + 0.5 < next) { double w = next - ms; if (w > 1.5) Sleep((DWORD)(w - 1)); else Sleep(0); continue; }
    if (SafeRead(cur.data(), r->addr, r->len)) {
      std::lock_guard<std::mutex> l(r->m);
      mdf::Feed(r->cells, prev.data(), cur.data(), r->len, (long long)ms); prev.swap(cur);
    }
    r->samples++; next += period; if (next < ms - period * 4) next = ms + period;
  }
  r->done = true; timeEndPeriod(1); return 0;
}

static void MemReport(MemRun* r, Out& o) {
  std::vector<mdf::Run> runs; long long total = 0;
  { std::lock_guard<std::mutex> l(r->m); runs = mdf::MergeRuns(r->cells); for (auto& c : r->cells) total += c.changes; }
  o.f("memdiff %s 0x%llx len %d %ds @%dHz%s: %zu changed runs, %lld byte changes, %ld samples%s", r->id.c_str(), (unsigned long long)r->addr, r->len, r->durMs / 1000, r->hz,
      r->done ? "" : " (running)", runs.size(), total, r->samples.load(), r->err.empty() ? "" : (" error=" + r->err).c_str());
  std::vector<std::string> js;
  for (auto& u : runs) {
    std::string prop = r->isObj ? PropAtOffset(r->addr, u.off) : "";
    std::string line = Fmt("+0x%x len=%d changes=%d first=%s->%s last=%s t=%lld-%lldms%s", u.off, u.len, u.changes, mdf::HexOf(u.firstOld).c_str(), mdf::HexOf(u.firstNew).c_str(),
                           mdf::HexOf(u.last).c_str(), u.t0, u.t1, mdf::Typed(u).c_str());
    if (!prop.empty()) line += " prop=" + prop;
    o.line(line);
    js.push_back(jo::Obj().num("off", u.off).num("len", u.len).num("changes", u.changes).str("first_old", mdf::HexOf(u.firstOld)).str("first_new", mdf::HexOf(u.firstNew)).str("last", mdf::HexOf(u.last)).str("prop", prop).num("t0", u.t0).num("t1", u.t1).done());
  }
  JsonSetData(jo::Obj().str("id", r->id).hex("addr", r->addr).num("len", r->len).num("samples", r->samples.load()).b("done", r->done).raw("runs", jo::Arr(js)).done());
}

static std::shared_ptr<MemRun> FindMemRun(const std::string& id) {
  if (id.empty()) return g_mruns.empty() ? nullptr : g_mruns.back();
  for (auto& r : g_mruns) if (r->id == id) return r;
  return nullptr;
}

static void CmdMemdiff(const std::vector<std::string>& t, Out& o) {
  std::string sub = t.size() > 1 ? Lower(t[1]) : "";
  if (sub == "result" || sub == "list" || sub == "stop") {
    std::lock_guard<std::mutex> l(g_mm);
    if (sub == "list") {
      std::vector<std::string> js;
      for (auto& r : g_mruns) { o.f("%s %s 0x%llx len=%d hz=%d samples=%ld", r->id.c_str(), r->done ? "done" : "running", (unsigned long long)r->addr, r->len, r->hz, r->samples.load()); js.push_back(jo::Obj().str("id", r->id).b("done", r->done).hex("addr", r->addr).num("len", r->len).num("samples", r->samples.load()).done()); }
      JsonSetData(jo::Obj().raw("memdiffs", jo::Arr(js)).done()); o.f("%zu memdiffs", g_mruns.size()); return;
    }
    if (sub == "stop") {
      int n = 0; for (auto& r : g_mruns) if (!r->done && (t.size() < 3 || t[2] == "all" || t[2] == r->id)) { r->stop = true; n++; }
      o.f("stopping %d memdiff(s)", n); return;
    }
    std::shared_ptr<MemRun> r = FindMemRun(t.size() > 2 ? t[2] : "");
    if (!r) { o.line("no such memdiff"); return; }
    MemReport(r.get(), o); return;
  }
  if (t.size() < 4) { o.line("usage: memdiff <addr|obj|obj+off> <len|auto> <secs> [hz=20] [name=<tag>] [wait]   |  memdiff result|list|stop [id]"); return; }
  std::string err; uint64_t a;
  if (!EvalAddr(t[1], a, err)) { o.line("error: " + err); return; }
  uint64_t len = 0; bool autoLen = t[2] == "auto";
  size_t osz = ObjSize((uintptr_t)a);
  if (autoLen) { if (!osz) { o.line("error: len auto needs an object"); return; } len = osz; }
  else if (!ParseU64(t[2], len)) { o.line("error: bad length " + t[2]); return; }
  int secs = atoi(t[3].c_str()), hz = 20; std::string name = "m"; bool wait = false;
  for (size_t i = 4; i < t.size(); i++) { if (t[i].rfind("hz=", 0) == 0) hz = atoi(t[i].c_str() + 3); else if (t[i].rfind("name=", 0) == 0) name = t[i].substr(5); else if (t[i] == "wait") wait = true; }
  if (len < 1 || len > 65536) { o.line("error: len 1-65536"); return; }
  if (hz < 1 || hz > 200 || secs < 1 || secs > 600) { o.line("error: hz 1-200, secs 1-600"); return; }
  { uint8_t probe[1]; if (!SafeRead(probe, a, 1) || !SafeRead(probe, a + len - 1, 1)) { o.f("error: cannot read %llu bytes at 0x%llx", (unsigned long long)len, (unsigned long long)a); return; } }
  std::unique_lock<std::mutex> l(g_mm);
  int running = 0; for (auto& r : g_mruns) if (!r->done) running++;
  if (running >= 4) { o.line("error: 4 memdiffs already running (memdiff stop)"); return; }
  while (g_mruns.size() >= 8) { auto it = std::find_if(g_mruns.begin(), g_mruns.end(), [](const std::shared_ptr<MemRun>& r) { return r->done.load(); }); if (it == g_mruns.end()) break; if ((*it)->th) CloseHandle((*it)->th); g_mruns.erase(it); }
  auto r = std::make_shared<MemRun>();
  r->id = "m" + std::to_string(++g_mrunId); r->name = name; r->addr = (uintptr_t)a; r->len = (int)len; r->hz = hz; r->durMs = secs * 1000; r->isObj = osz > 0;
  r->cells.assign((size_t)len, mdf::Cell());
  std::shared_ptr<MemRun> raw = r; raw->th = CreateThread(nullptr, 0, MemLoop, raw.get(), 0, nullptr);
  g_mruns.push_back(std::move(r));
  o.f("memdiff %s started: 0x%llx len %d, %d Hz, %d s", raw->id.c_str(), (unsigned long long)a, raw->len, hz, secs);
  if (!wait) return;
  l.unlock();
  ULONGLONG until = GetTickCount64() + (ULONGLONG)(std::min(secs, 65) + 5) * 1000;
  while (!raw->done && GetTickCount64() < until) Sleep(50);
  l.lock();
  if (!raw->err.empty()) { o.line("error: " + raw->err); return; }
  MemReport(raw.get(), o);
}

void InitProbe() {}
void ShutdownProbe() {
  { std::lock_guard<std::mutex> l(g_mm);
    for (auto& r : g_mruns) r->stop = true;
    for (auto& r : g_mruns) if (r->th) { WaitForSingleObject(r->th, 3000); if (r->done) { CloseHandle(r->th); r->th = nullptr; } }
    for (auto& r : g_mruns) if (!r->done) new std::shared_ptr<MemRun>(r);
    g_mruns.clear(); }
  std::lock_guard<std::mutex> l(g_sm);
  for (auto& r : g_runs) r->stop = true;
  for (auto& r : g_runs) if (r->th) { WaitForSingleObject(r->th, 3000); if (r->done) { CloseHandle(r->th); r->th = nullptr; } }
  for (auto& r : g_runs) if (!r->done) new std::shared_ptr<smp::Run>(r);
  g_runs.clear();
}

bool HandleProbe(const std::string& c, const std::vector<std::string>& t, const std::string& line, Out& o) {
  if (c == "sample") { CmdSample(t, o); return true; }
  if (c == "memdiff") { CmdMemdiff(t, o); return true; }
  return false;
}
