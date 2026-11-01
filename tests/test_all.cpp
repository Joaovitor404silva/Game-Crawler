#include "core.h"
#include "json_out.h"
#include "agg.h"
#include "sampler.h"
#include "stackwalk.h"
#include "stats.h"
#include "opts.h"
#include "memdiff.h"
#include "retry.h"
#include "pose.h"
#include "containers.h"
#include "plug_restore.h"
#include "hwhook.h"
#include <map>
#include <thread>
#include <atomic>
#include <stdio.h>
#include "ue_scan.h"
#include "scan.h"

extern HostApi g_testApi;
extern std::vector<std::string> g_testLog;
extern "C" void CoreHandle(const char* line, EmitFn emit, void* ctx);
extern "C" int CoreInit(HostApi* api);
extern "C" void CoreShutdown();

static int g_fail, g_pass;
#define CHECK(c) do { if (c) g_pass++; else { g_fail++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static std::string Run(const std::string& cmd) {
  std::string r; CoreHandle(cmd.c_str(), [](void* c, const char* t) { *(std::string*)c += t; }, &r); return r;
}
static std::string Hx(uint64_t v) { return Fmt("0x%llx", (unsigned long long)v); }

alignas(64) static volatile LONG64 g_target[8];
alignas(64) static volatile LONG g_counter[4];
static volatile int g_sink;

#define NOINL __declspec(noinline)
NOINL void WriterA(LONG64 v) { g_target[0] = v; }
NOINL void WriterB(LONG64 v) { g_target[0] = v + 1; }
NOINL void WriterWide(LONG64 v) { for (int i = 0; i < 4; i++) g_target[i] = v + i; }
NOINL void CallerOne(LONG64 v) { WriterA(v); g_sink++; }
NOINL void CallerTwo(LONG64 v) { WriterB(v); g_sink++; }
NOINL int HotFn(int x) { g_sink = x; return x + 1; }
NOINL void HotCallerA(int x) { HotFn(x); g_sink++; }
NOINL void HotCallerB(int x) { HotFn(x); g_sink--; }
NOINL void Deep3(uint64_t* out, int* n) { CONTEXT c; RtlCaptureContext(&c); *n = WalkStack(c, out, 8); }
NOINL void Deep2(uint64_t* out, int* n) { Deep3(out, n); g_sink++; }
NOINL void Deep1(uint64_t* out, int* n) { Deep2(out, n); g_sink++; }

static uint64_t FnBegin(void* f) { DWORD64 ib; auto rf = RtlLookupFunctionEntry((DWORD64)f, &ib, nullptr); return rf ? ib + rf->BeginAddress : (uint64_t)f; }

static void TestJsonOut() {
  CHECK(jo::Esc("a\"b\\c\n\t") == "a\\\"b\\\\c\\n\\t");
  CHECK(jo::Esc(std::string("x\x01y")) == "x\\u0001y");
  CHECK(jo::Esc("caf\xc3\xa9") == "caf\xc3\xa9");
  CHECK(jo::Esc("bad\xff" "x") == "bad?x");
  CHECK(jo::Esc("trunc\xe2\x82") == "trunc??");
  CHECK(jo::Q("a") == "\"a\"");
  CHECK(jo::Hex(255) == "\"0xff\"");
  CHECK(jo::Dbl(1.5) == "1.5");
  CHECK(jo::Dbl(0.0 / 0.0 * 0 + (1e308 * 10)) == "null");
  CHECK(jo::Obj().str("a", "b").num("n", 3).b("t", true).done() == "{\"a\":\"b\",\"n\":3,\"t\":true}");
  CHECK(jo::Arr({ "1", "2" }) == "[1,2]");
  CHECK(jo::LooksLikeError("error: bad term 'x'"));
  CHECK(jo::LooksLikeError("usage: rd <addr>"));
  CHECK(jo::LooksLikeError("cannot resolve x"));
  CHECK(!jo::LooksLikeError("0x7ff6  Actor"));
  CHECK(!jo::LooksLikeError("bp0 write 0x10 len 4"));
  auto rd = jo::StructureRd({ "00007ff600001000  48 8b 05 11 22 33 44 55 66 77 88 99 aa bb cc dd  H.....", "00007ff600001010  01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f 10  ........" });
  CHECK(rd.find("\"hex\":\"488b0511223344556677889" "9aabbccdd0102030405060708090a0b0c0d0e0f10\"") != std::string::npos);
  CHECK(rd.find("\"len\":32") != std::string::npos);
  auto ptr = jo::StructurePtr({ "0x1000  [exe+0x10 Foo]  -> qword 0x2a float 5.88545e-44 double 2.07508e-322 int 42" });
  CHECK(ptr.find("\"sym\":\"exe+0x10 Foo\"") != std::string::npos && ptr.find("\"int\":42") != std::string::npos);
  auto get = jo::StructureGet({ "FloatProperty CharacterMovement.MaxWalkSpeed @0x7ff61234 = 600" });
  CHECK(get.find("\"path\":\"CharacterMovement.MaxWalkSpeed\"") != std::string::npos && get.find("\"value\":\"600\"") != std::string::npos);
  auto hk = jo::StructureHooks({ "1  0x7ff600001000  Agent::IsInThreat(impl)               hits=12 mode=log agg groups=3 filtered=0", "1 hooks" });
  CHECK(hk.find("\"hits\":12") != std::string::npos && hk.find("\"label\":\"Agent::IsInThreat(impl)\"") != std::string::npos);
  auto bp = jo::StructureBps({ "bp1 write 0x7ff600002000 len 4 hits=9 agg groups=2" });
  CHECK(bp.find("\"slot\":1") != std::string::npos && bp.find("\"hits\":9") != std::string::npos);
  auto fd = jo::StructureFind({ "0x7ff600001000  Pawn   /Game/Foo.Foo", "1 hits" });
  CHECK(fd.find("\"class\":\"Pawn\"") != std::string::npos && fd.find("\"total\":1") != std::string::npos);
  auto env = jo::Envelope("rd", true, { "a", "b\"c" }, "{\"x\":1}", 27063, 5);
  CHECK(env == "{\"ok\":true,\"cmd\":\"rd\",\"port\":27063,\"ms\":5,\"data\":{\"x\":1},\"lines\":[\"a\",\"b\\\"c\"]}");
}

static void TestAgg() {
  agg::Table t;
  uint64_t s1[3] = { 0x10, 0x20, 0x30 }, s2[3] = { 0x10, 0x20, 0x31 };
  bool nw = false;
  int a = t.Hit(s1, 3, 0, 0, 1, 2, 7, 100, &nw); CHECK(nw && a >= 0);
  int b = t.Hit(s1, 3, 0, 0, 2, 3, 7, 101, &nw); CHECK(!nw && b == a);
  int c = t.Hit(s2, 3, 0, 0, 3, 4, 8, 102, &nw); CHECK(nw && c != a);
  int d = t.Hit(s1, 3, 1, 0, 3, 4, 8, 102, &nw); CHECK(nw && d != a);
  CHECK(t.used == 3 && t.total == 4);
  CHECK(t.g[a].hits == 2 && t.g[a].firstOld == 1 && t.g[a].firstNew == 2 && t.g[a].lastOld == 2 && t.g[a].lastNew == 3);
  auto top = t.Top(0); CHECK(top.size() == 3 && top[0] == a);
  CHECK(t.Top(1).size() == 1);
  t.Clear(); CHECK(t.used == 0 && t.Top(0).empty());
  int N = 8; std::vector<std::thread> th;
  for (int i = 0; i < N; i++) th.emplace_back([&, i] {
    for (int k = 0; k < 100000; k++) { uint64_t f[2] = { 0x1000, (uint64_t)(k % 50) }; bool n2; t.Hit(f, 2, 0, 0, 0, 0, i, k, &n2); }
  });
  for (auto& x : th) x.join();
  CHECK(t.used == 50 && t.total == 800000);
  int64_t sum = 0; for (int gi : t.Top(0)) sum += t.g[gi].hits; CHECK(sum == 800000);
  agg::Table small; int ov = 0;
  for (int k = 0; k < agg::Table::CAP + 100; k++) { uint64_t f[1] = { (uint64_t)k + 1 }; bool n2; if (small.Hit(f, 1, 0, 0, 0, 0, 0, 0, &n2) < 0) ov++; }
  CHECK(small.used == agg::Table::CAP && ov >= 100 && small.overflow >= 100);
}

static void TestChunk() {
  auto c1 = agg::Chunk(0x1000, 8); CHECK(c1.size() == 1 && c1[0].second == 8);
  auto c2 = agg::Chunk(0x1004, 8); CHECK(c2.size() == 2 && c2[0].second == 4 && c2[1].second == 4);
  auto c3 = agg::Chunk(0x1001, 3); CHECK(c3.size() == 2 && c3[0].second == 1 && c3[1].second == 2);
  auto c4 = agg::Chunk(0x1000, 32); CHECK(c4.size() == 4);
  auto c5 = agg::Chunk(0x1003, 11);
  size_t sum = 0; for (auto& p : c5) { CHECK(p.first % p.second == 0); sum += p.second; } CHECK(sum == 11);
}

static void TestSampler() {
  std::vector<smp::Col> cols; std::string err;
  CHECK(smp::ParseCols("x=[pawn+0x1c8]:f32,hp=@pawn|Health.Cur,0x7ff600001000:u16,[a+8]+4", cols, err));
  CHECK(cols.size() == 4);
  CHECK(cols[0].name == "x" && cols[0].expr == "[pawn+0x1c8]" && cols[0].type == smp::F32);
  CHECK(cols[1].name == "hp" && cols[1].isProp && cols[1].obj == "pawn" && cols[1].path == "Health.Cur");
  CHECK(cols[2].name == "c2" && cols[2].type == smp::U16 && cols[2].expr == "0x7ff600001000");
  CHECK(cols[3].name == "c3" && cols[3].type == smp::F32 && cols[3].expr == "[a+8]+4");
  cols.clear(); CHECK(!smp::ParseCols("a=1:zzz", cols, err) && err.find("unknown type") != std::string::npos);
  cols.clear(); CHECK(!smp::ParseCols("a=@pawn", cols, err));
  cols.clear(); CHECK(!smp::ParseCols("", cols, err));
  float f = 1.5f; uint8_t b[8]; memcpy(b, &f, 4);
  CHECK(smp::FormatValue(smp::F32, b) == "1.5");
  int16_t s = -5; memcpy(b, &s, 2); CHECK(smp::FormatValue(smp::I16, b) == "-5");
  uint64_t q = 0xdeadbeef; memcpy(b, &q, 8); CHECK(smp::FormatValue(smp::HEX, b) == "0xdeadbeef");
  CHECK(smp::Header({ cols.size() ? cols[0] : smp::Col{} }).rfind("tick\tms", 0) == 0);
}

static void TestWalk() {
  uint64_t fr[8]; int n = 0;
  Deep1(fr, &n);
  CHECK(n >= 4);
  CHECK(FnBegin((void*)fr[0]) == FnBegin((void*)&Deep3));
  uint64_t r1 = fr[1] - 1, r2 = fr[2] - 1;
  CHECK(FnBegin((void*)r1) == FnBegin((void*)&Deep2));
  CHECK(FnBegin((void*)r2) == FnBegin((void*)&Deep1));
}

alignas(16) static uint8_t g_jmpBuf[64];

static void TestDetour() {
  memset(g_jmpBuf, 0x90, sizeof g_jmpBuf);
  g_jmpBuf[0] = 0xE9; int32_t rel = 0x20; memcpy(g_jmpBuf + 1, &rel, 4);
  uintptr_t base = (uintptr_t)g_jmpBuf;
  CHECK(DetourTarget(base) == base + 5 + 0x20);
  CHECK(!DetourOwner(base).empty());
  uint8_t plain[16] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20 };
  CHECK(DetourTarget((uintptr_t)plain) == 0);
  CHECK(DetourOwner((uintptr_t)plain).empty());
  uint8_t mov[16] = { 0x48, 0xB8 }; uint64_t imm = 0x123456789ull; memcpy(mov + 2, &imm, 8); mov[10] = 0xFF; mov[11] = 0xE0;
  CHECK(DetourTarget((uintptr_t)mov) == imm);
  uint8_t ind[24] = { 0xFF, 0x25 }; int32_t r2 = 6; memcpy(ind + 2, &r2, 4); uint64_t tgt = 0xabc000; memcpy(ind + 12, &tgt, 8);
  CHECK(DetourTarget((uintptr_t)ind) == tgt);
}

static void TestStats() {
  cst::Clear(); cst::Add("find", 10); cst::Add("find", 30); cst::Add("rd", 1); cst::Add("", 5);
  auto rows = cst::Rows();
  CHECK(rows.size() == 2 && rows[0].cmd == "find" && rows[0].count == 2 && rows[0].total == 40 && rows[0].max == 30);
  cst::Clear(); CHECK(cst::Rows().empty());
}

static void TestUopt() {
  auto a = uopt::Parse({ "find", "isa:X", "10", "near=1,2,3", "r=300", "loc", "new=2" }, 2);
  CHECK(a.pos.size() == 1 && a.pos[0] == "10" && a.Str("near") == "1,2,3" && a.Int("r", 0) == 300 && a.Flag("loc") && a.Int("new", 0) == 2 && !a.Has("hidden"));
  double v[3]; CHECK(uopt::ParseXyz("1,2.5,-3", v) && v[0] == 1 && v[1] == 2.5 && v[2] == -3);
  CHECK(!uopt::ParseXyz("pawn", v) && !uopt::ParseXyz("1,2", v) && !uopt::ParseXyz("1,2,3,4", v));
  double p[3] = { 0, 0, 0 }, q[3] = { 3, 4, 0 }; CHECK(uopt::Dist(p, q) == 5.0);
  std::vector<uopt::Row> rows(3); rows[0].idx = 5; rows[0].dist = 30; rows[1].idx = 9; rows[1].dist = 10; rows[2].idx = 7; rows[2].dist = 20;
  auto byDist = rows; uopt::SelectRows(byDist, true, 0); CHECK(byDist[0].dist == 10 && byDist[2].dist == 30);
  auto byNew = rows; uopt::SelectRows(byNew, false, 2); CHECK(byNew.size() == 2 && byNew[0].idx == 9 && byNew[1].idx == 7);
}

static void TestPose() {
  double q0[4] = { 0, 0, 0, 1 }, e[3];
  pse::Euler(q0, e); CHECK(fabs(e[0]) < 1e-9 && fabs(e[1]) < 1e-9 && fabs(e[2]) < 1e-9);
  double s = sin(3.14159265358979 / 4), c = cos(3.14159265358979 / 4);
  double qz90[4] = { 0, 0, s, c }; pse::Euler(qz90, e); CHECK(fabs(e[2] - 90.0) < 1e-6 && fabs(e[0]) < 1e-6 && fabs(e[1]) < 1e-6);
  double qx90[4] = { s, 0, 0, c }; pse::Euler(qx90, e); CHECK(fabs(fabs(e[0]) - 90.0) < 1e-6);
  CHECK(fabs(pse::QuatAngle(q0, qz90) - 90.0) < 1e-6);
  CHECK(pse::QuatAngle(q0, q0) < 1e-6);
  CHECK(fabs(pse::Wrap180(190.0) + 170.0) < 1e-9 && fabs(pse::Wrap180(-190.0) - 170.0) < 1e-9);
  CHECK(pse::TwistByRoll(130.0) && pse::TwistByRoll(-150.0) && !pse::TwistByRoll(105.0) && !pse::TwistByRoll(-104.0));
  CHECK(pse::Hover(3.5) && !pse::Hover(3.0) && pse::CompFlag(18.2, true) == "HOVER" && pse::CompFlag(1.0, true).empty() && pse::CompFlag(18.2, false).empty());
  std::vector<pse::Row> rows(3);
  rows[0].bone = "jnt_r_hand"; rows[1].bone = "jnt_r_fore_arm"; rows[2].bone = "jnt_l_fore_arm";
  rows[0].par.q[0] = 0; double h130[4] = { sin(130.0 * 3.14159265358979 / 360.0), 0, 0, cos(130.0 * 3.14159265358979 / 360.0) };
  for (int i = 0; i < 4; i++) rows[0].par.q[i] = h130[i];
  pse::Finish(rows);
  CHECK(rows[1].flag == "TWIST" && rows[0].flag.empty() && rows[2].flag.empty());
  std::vector<pse::Row> r2(1); r2[0].bone = "jnt_r_arm"; r2[0].haveRef = true; r2[0].refPar = pse::Xf(); r2[0].par = pse::Xf();
  double q40[4] = { sin(40.0 * 3.14159265358979 / 360.0), 0, 0, cos(40.0 * 3.14159265358979 / 360.0) };
  for (int i = 0; i < 4; i++) r2[0].par.q[i] = q40[i];
  r2[0].comp.t[0] = 3; r2[0].comp.t[1] = 4;
  pse::Finish(r2);
  CHECK(r2[0].flag == "TWIST" && fabs(r2[0].dRot - 40.0) < 1e-6 && fabs(r2[0].dLoc - 5.0) < 1e-9);
  CHECK(pse::BoneSet("").empty() && pse::IsPreset("") && pse::IsPreset("arms") && !pse::IsPreset("hand_r"));
  auto cu = pse::BoneSet("a,b,,c"); CHECK(cu.size() == 3 && cu[2] == "c");
  CHECK(pse::InPreset("jnt_l_thigh", "legs") && pse::InPreset("jnt_r_toe", "legs") && !pse::InPreset("jnt_r_hand", "legs") && !pse::InPreset("pelvis", "legs"));
  CHECK(pse::InPreset("hand_r", "hands") && pse::InPreset("LowerArm_L", "hands") && !pse::InPreset("upperarm_l", "hands") && pse::InPreset("upperarm_l", "arms"));
  CHECK(pse::InPreset("jnt_r_fore_arm", "hands") && pse::InPreset("jnt_l_arm", "arms") && !pse::InPreset("lowerarm_twist_01_l", "hands") && !pse::InPreset("ik_hand_r", "hands"));
}

static void TestContainers() {
  const uint32_t stride = cnt::SparseStride(8, 8);
  CHECK(stride == 16);
  std::vector<uint8_t> mem(4096, 0);
  uint64_t base = 0x1000, dataAddr = 0x2000 - 0x1000;
  auto put = [&](uint64_t off, const void* p, size_t n) { memcpy(&mem[off], p, n); };
  uint64_t dp = 0x10000 + dataAddr; int32_t slots = 7, nbits = 7, nfree = 2;
  uint64_t sec = 0; uint32_t bits = 0;
  for (int i : {0, 1, 3, 4, 6}) bits |= 1u << i;
  put(base - 0x1000 + 0, &dp, 8); put(8, &slots, 4); put(16, &bits, 4); put(32, &sec, 8); put(40, &nbits, 4); put(52, &nfree, 4);
  cnt::ReadFn rd = [&](uint64_t a, void* o, size_t n) { if (a < 0x1000 || a + n > 0x1000 + mem.size()) return false; memcpy(o, &mem[a - 0x1000], n); return true; };
  std::vector<uint64_t> out; cnt::SparseInfo si;
  CHECK(cnt::SparseElems(rd, 0x1000, cnt::Layout(), stride, 100, out, &si));
  CHECK(out.size() == 5);
  CHECK(out.size() == 5 && out[0] == dp && out[1] == dp + 16 && out[2] == dp + 48 && out[3] == dp + 64 && out[4] == dp + 96);
  CHECK(si.slots == 7 && si.free == 2);
  std::vector<uint64_t> lim; CHECK(cnt::SparseElems(rd, 0x1000, cnt::Layout(), stride, 2, lim) && lim.size() == 2);
  int32_t bad = 9; put(40, &bad, 4); put(8, &bad, 4); slots = 9;
  std::vector<uint64_t> o2; CHECK(cnt::SparseElems(rd, 0x1000, cnt::Layout(), stride, 100, o2));
  int32_t neg = -1; put(8, &neg, 4);
  std::vector<uint64_t> o3; CHECK(!cnt::SparseElems(rd, 0x1000, cnt::Layout(), stride, 100, o3));
  std::vector<uint64_t> o4; CHECK(!cnt::SparseElems([](uint64_t, void*, size_t) { return false; }, 0x1000, cnt::Layout(), stride, 100, o4));
  CHECK(cnt::InnerSlot("ArrayProperty") == 1 && cnt::InnerSlot("SetProperty") == 0 && cnt::InnerSlot("MapProperty") == 0 && cnt::InnerSlot(nullptr) == 0);
}

static void TestPlugRestore() {
  CHECK(plr::Join({ "nbot", "", "x" }) == "nbot,x" && plr::Join({}).empty());
  auto s = plr::Split("nbot,x,,nbot"); CHECK(s.size() == 2 && s[0] == "nbot" && s[1] == "x");
  CHECK(plr::Split("").empty());
  auto m = plr::Missing({ "a", "b", "c" }, { "b" }); CHECK(m.size() == 2 && m[0] == "a" && m[1] == "c");
}

struct FakeMem {
  std::map<uint64_t, std::vector<uint8_t>> r;
  uint8_t* At(uint64_t a, size_t n) { for (auto& kv : r) if (a >= kv.first && a + n <= kv.first + kv.second.size()) return kv.second.data() + (a - kv.first); return nullptr; }
  void Put(uint64_t a, const void* p, size_t n) { memcpy(At(a, n), p, n); }
  template <class T> void Put(uint64_t a, T v) { Put(a, &v, sizeof v); }
  ues::ReadFn Fn() { return [this](uint64_t a, void* o, size_t n) { uint8_t* p = At(a, n); if (!p) return false; memcpy(o, p, n); return true; }; }
};
static void PutEntry(FakeMem& m, uint64_t at, const char* s, bool wide = false) {
  uint16_t len = (uint16_t)strlen(s), h = (uint16_t)((len << 6) | (wide ? 1 : 0)); m.Put(at, h);
  for (uint16_t i = 0; i < len; i++) { if (wide) m.Put<uint16_t>(at + 2 + i * 2, (uint16_t)s[i]); else m.Put<uint8_t>(at + 2 + i, (uint8_t)s[i]); }
}
static void TestScan() {
  using namespace scn;
  CHECK(ParseType("f32") == F32 && ParseType("int") == I32 && ParseType("x") == BAD && ParseOp("dec") == DEC && ParseOp("?") == OPBAD);
  uint8_t buf[64] = {}; int32_t v = 1234; memcpy(buf + 8, &v, 4); memcpy(buf + 13, &v, 4); float fl = 7.5f; memcpy(buf + 20, &fl, 4);
  memcpy(buf + 40, "money", 5);
  std::vector<Hit> h; Val w; CHECK(ParseVal(I32, "1234", w));
  ScanBlock(I32, buf, sizeof buf, 0x1000, w, 0, "", h, 100);
  CHECK(h.size() == 1 && h[0].addr == 0x1008);
  h.clear(); ScanBlock(I32, buf, sizeof buf, 0x1003, w, 0, "", h, 100);
  CHECK(h.size() == 1 && h[0].addr == 0x1003 + 13);
  h.clear(); CHECK(ParseVal(F32, "7.504", w)); ScanBlock(F32, buf, sizeof buf, 0x1000, w, 0.01, "", h, 100);
  CHECK(h.size() == 1 && h[0].addr == 0x1014 && fabs(h[0].last.d - 7.5) < 1e-6);
  h.clear(); ScanBlock(STR, buf, sizeof buf, 0x1000, w, 0, "money", h, 100); CHECK(h.size() == 1 && h[0].addr == 0x1028);
  h.clear(); CHECK(ParseVal(I8, "0", w)); ScanBlock(I8, buf, sizeof buf, 0, w, 0, "", h, 5); CHECK(h.size() == 5);
  Val a, b, z; a.i = 5; a.d = 5; b.i = 7; b.d = 7;
  CHECK(Keep(I32, INC, a, b, z, 0) && !Keep(I32, DEC, a, b, z, 0) && Keep(I32, CHANGED, a, b, z, 0) && !Keep(I32, UNCHANGED, a, b, z, 0));
  uint8_t out[8]; Val sv; sv.d = 2.5; CHECK(Store(F32, sv, out)); float back; memcpy(&back, out, 4); CHECK(back == 2.5f);
  CHECK(Fmt(F32, sv) == "2.5" && !ParseVal(I32, "12x", w));
}

static void TestUeScan() {
  FakeMem m; m.r[0x1000] = std::vector<uint8_t>(0x200); m.r[0x100000] = std::vector<uint8_t>(0x100); m.r[0x200000] = std::vector<uint8_t>(0x200); m.r[0x300000] = std::vector<uint8_t>(0x800); m.r[0x400000] = std::vector<uint8_t>(0x100);
  uint64_t arr = 0x1000 + 0x40;
  m.Put<uint64_t>(arr + 0x10, 0x100000); m.Put<int>(arr + 0x20, 0x20000); m.Put<int>(arr + 0x24, 5); m.Put<int>(arr + 0x28, 2); m.Put<int>(arr + 0x2C, 1);
  m.Put<uint64_t>(0x100000, 0x200000);
  for (int i = 0; i < 5; i++) { m.Put<uint64_t>(0x200000 + i * 24, 0x300000 + i * 0x100); m.Put<int>(0x300000 + i * 0x100 + 0xC, i); }
  auto rd = m.Fn();
  auto h = ues::FindObjArray(m.At(0x1000, 0x200), 0x200, 0x1000, rd);
  CHECK(h.va == arr && h.num == 5 && h.max == 0x20000 && h.stride == 24);
  m.Put<int>(arr + 0x28, 3); CHECK(ues::FindObjArray(m.At(0x1000, 0x200), 0x200, 0x1000, rd).va == 0); m.Put<int>(arr + 0x28, 2);
  m.Put<int>(arr + 0x2C, 2); CHECK(ues::FindObjArray(m.At(0x1000, 0x200), 0x200, 0x1000, rd).va == arr); m.Put<int>(arr + 0x2C, 3); CHECK(ues::FindObjArray(m.At(0x1000, 0x200), 0x200, 0x1000, rd).va == 0); m.Put<int>(arr + 0x2C, 1);
  m.Put<int>(0x300000 + 2 * 0x100 + 0xC, 7); CHECK(ues::FindObjArray(m.At(0x1000, 0x200), 0x200, 0x1000, rd).va == 0); m.Put<int>(0x300000 + 2 * 0x100 + 0xC, 2);
  PutEntry(m, 0x400000, "None"); PutEntry(m, 0x400006, "ByteProperty"); PutEntry(m, 0x400014, "Actor"); PutEntry(m, 0x40001C, "Ab", true);
  uint64_t blocks = 0x1000 + 0x100; m.Put<uint64_t>(blocks, 0x400000); m.Put<uint32_t>(blocks - 8, 0);
  CHECK(ues::FindNameBlocks(m.At(0x1000, 0x200), 0x200, 0x1000, rd) == blocks);
  std::string s;
  CHECK(ues::NameAt(rd, blocks, 0, s) && s == "None");
  CHECK(ues::NameAt(rd, blocks, 3, s) && s == "ByteProperty");
  CHECK(ues::NameAt(rd, blocks, 10, s) && s == "Actor");
  CHECK(ues::NameAt(rd, blocks, 14, s) && s == "Ab");
  CHECK(!ues::NameAt(rd, blocks, 1u << 16, s));
  std::unordered_map<std::string, uint32_t> pool; ues::IndexPool(rd, blocks, pool);
  CHECK(pool.size() == 4 && pool["actor"] == 10 && pool["ab"] == 14);
  uint64_t fn = 0;
  CHECK(ues::LookupName(pool, "ACTOR", fn) && fn == 10);
  CHECK(ues::LookupName(pool, "Actor_5", fn) && fn == (10ull | (6ull << 32)));
  CHECK(ues::LookupName(pool, "Actor_0", fn) && fn == (10ull | (1ull << 32)));
  CHECK(!ues::LookupName(pool, "Actor_05", fn) && !ues::LookupName(pool, "Nope", fn) && !ues::LookupName(pool, "Actor_", fn));
  uint64_t vt = 0x400080; for (int i = 0; i < 4; i++) m.Put<uint64_t>(vt + i * 8, 0x7000 + i * 0x100); m.Put<uint64_t>(vt + 32, 0);
  ues::DecodeFn dec = [](uint64_t at, ues::Insn& i) {
    i = {}; i.len = 4;
    if (at == 0x7200 + 8) { i.op = 0xF7; i.mem = true; i.modrm = 0x80; i.imm = 0x400; i.disp = 0xB0; }
    else if (at == 0x7100 + 8) { i.op = 0xF7; i.mem = false; i.modrm = 0xC0; i.imm = 0x400; i.disp = 0xB0; }
    else if ((at & 0xFF) >= 0x40) i.op = 0xCC; else i.op = 0x90;
    return true; };
  std::vector<int> c;
  CHECK(ues::FindProcessEventSlot(rd, dec, vt, 0x7000, 0x8000, 0xB0, &c) == 2 && c.size() == 1 && c[0] == 2);
  CHECK(ues::FindProcessEventSlot(rd, dec, vt, 0x7000, 0x7200, 0xB0) == -1);
  CHECK(ues::FindProcessEventSlot(rd, dec, vt, 0x7000, 0x8000, 0x88) == -1);
  ues::DecodeFn decB = [](uint64_t at, ues::Insn& i) {
    i = {}; i.len = 4;
    if (at == 0x7300 + 8) { i.op = 0xF6; i.mem = true; i.modrm = 0x83; i.imm = 4; i.disp = 0xB1; }
    else if (at == 0x7100 + 8) { i.op = 0xF6; i.mem = true; i.modrm = 0x83; i.imm = 4; i.disp = 0x31; }
    else if ((at & 0xFF) >= 0x40) i.op = 0xCC; else i.op = 0x90;
    return true; };
  CHECK(ues::FindProcessEventSlot(rd, decB, vt, 0x7000, 0x8000, 0xB0) == 3);
}
static void TestHwHook() {
  hwh::Slot s[4]; s[0].used = true; s[0].hookId = 7; s[1].used = true; s[2].used = true; s[2].hookId = 9; s[3].hookId = 5;
  CHECK(hwh::Pick(s, 4, -1) == 0x5u && hwh::Count(hwh::Pick(s, 4, -1)) == 2);
  CHECK(hwh::Pick(s, 4, 9) == 0x4u && hwh::Pick(s, 4, 7) == 0x1u);
  CHECK(hwh::Pick(s, 4, 5) == 0 && hwh::Pick(s, 4, 99) == 0);
  CHECK(hwh::Count(0) == 0 && hwh::Count(0xf) == 4);
}

static void TestMemdiff() {
  std::vector<mdf::Cell> cells(0x50);
  uint8_t a[0x50] = {}, b[0x50] = {}, c[0x50] = {};
  b[0x10] = 1; b[0x11] = 2; b[0x12] = 3; b[0x40] = 9;
  CHECK(mdf::Feed(cells, a, b, sizeof a, 100) == 4);
  c[0x10] = 5; c[0x11] = 2; c[0x12] = 3; c[0x40] = 9;
  CHECK(mdf::Feed(cells, b, c, sizeof a, 250) == 1);
  auto runs = mdf::MergeRuns(cells);
  CHECK(runs.size() == 2 && runs[0].off == 0x10 && runs[0].len == 3 && runs[1].off == 0x40 && runs[1].len == 1);
  CHECK(runs[0].changes == 2 && runs[0].t0 == 100 && runs[0].t1 == 250 && mdf::HexOf(runs[0].last) == "050203" && mdf::HexOf(runs[0].firstOld) == "000000");
  std::vector<mdf::Cell> fc(4); uint8_t f1[4], f2[4]; float x = 100.f, y = 92.f; memcpy(f1, &x, 4); memcpy(f2, &y, 4);
  uint8_t z[4] = {}; mdf::Feed(fc, f1, f2, 4, 5);
  auto fr = mdf::MergeRuns(fc); CHECK(!fr.empty() && fr[0].len >= 1);
  mdf::Run tr; tr.len = 4; tr.firstOld.assign(f1, f1 + 4); tr.last.assign(f2, f2 + 4);
  CHECK(mdf::Typed(tr).find("f32=100->92") != std::string::npos);
  (void)z;
}

static void TestRetry() {
  rty::Gate g;
  CHECK(g.Try(1000, 2000) && !g.Try(1500, 2000) && g.WaitSec(1500) == 2 && g.Try(3000, 2000));
  CHECK(g.Loud(1000, 30000) && !g.Loud(20000, 30000) && g.Loud(31001, 30000));
  g.Reset(); CHECK(g.Try(5, 2000));
}

static void Hex(const char* name, uint64_t v, FILE* f, bool& first) { fprintf(f, "%s\"%s\":\"0x%llx\"", first ? "" : ",", name, (unsigned long long)v); first = false; }

static void Record(FILE* f, const char* step, const std::string& cmd, bool both = false) {
  std::string text, js;
  if (both) { text = Run(cmd); js = Run("json " + cmd); } else { js = Run("json " + cmd); }
  fprintf(f, "{\"step\":%s,\"cmd\":%s,\"text\":%s,\"json\":", jo::Q(step).c_str(), jo::Q(cmd).c_str(), jo::Q(text).c_str());
  fprintf(f, "%s}\n", js.empty() ? "null" : js.substr(0, js.find('\n')).c_str());
}

static void Scenario(const char* path) {
  FILE* f = fopen(path, "wb");
  fprintf(f, "{\"step\":\"fns\",");
  bool first = true;
  Hex("WriterA", FnBegin((void*)&WriterA), f, first); Hex("WriterB", FnBegin((void*)&WriterB), f, first); Hex("WriterWide", FnBegin((void*)&WriterWide), f, first);
  Hex("CallerOne", FnBegin((void*)&CallerOne), f, first); Hex("CallerTwo", FnBegin((void*)&CallerTwo), f, first);
  Hex("HotFn", FnBegin((void*)&HotFn), f, first); Hex("HotCallerA", FnBegin((void*)&HotCallerA), f, first); Hex("HotCallerB", FnBegin((void*)&HotCallerB), f, first);
  Hex("target", (uint64_t)&g_target[0], f, first); Hex("counter", (uint64_t)&g_counter[0], f, first);
  fprintf(f, ",\"base\":\"0x%llx\"}\n", (unsigned long long)g_testApi.base);

  Record(f, "basic", "ver", true);
  Record(f, "basic", "rd " + Hx((uint64_t)&g_target[0]) + " 32", true);
  Record(f, "basic", "ptr " + Hx(FnBegin((void*)&HotFn)), true);
  Record(f, "basic", "sym " + Hx(FnBegin((void*)&HotFn)), true);
  Record(f, "basic", "rva " + Hx(FnBegin((void*)&HotFn)), true);
  Record(f, "basic", "hooks", true);
  Record(f, "basic", "bps", true);
  Record(f, "basic", "rd 0x10 8", true);
  Record(f, "basic", "nosuchcommand", true);
  Record(f, "basic", "mark scenario-start", true);
  Record(f, "basic", "scene", true);
  Record(f, "basic", "agg", true);
  Record(f, "basic", "ping", true);

  std::string out = Run("json status"); fprintf(f, "{\"step\":\"jsonstatus\",\"text\":%s}\n", jo::Q(out).c_str());

  Record(f, "hook", "hook " + Hx(FnBegin((void*)&HotFn)) + " agg stack=6 max=5");
  for (int i = 0; i < 300; i++) HotCallerA(i);
  for (int i = 0; i < 100; i++) HotCallerB(i);
  Record(f, "hook", "hooks");
  Record(f, "hook", "agg top=5");
  size_t logBefore = g_testLog.size();
  std::thread fl([] { for (int i = 0; i < 200000; i++) { HotCallerA(i); HotCallerB(i); } });
  fl.join();
  fprintf(f, "{\"step\":\"flood\",\"calls\":400000,\"newLogLines\":%zu,\"ret\":%d}\n", g_testLog.size() - logBefore, HotFn(41));
  Record(f, "hook", "agg top=5");
  Record(f, "hook", "unhook all");
  Record(f, "hook", "hook " + Hx(FnBegin((void*)&HotFn)) + " ret 77 agg rcx=5");
  int r5 = HotFn(5), r6 = HotFn(6), r7 = HotFn(5);
  fprintf(f, "{\"step\":\"filter\",\"r5\":%d,\"r6\":%d,\"r7\":%d}\n", r5, r6, r7);
  Record(f, "hook", "hooks");
  Record(f, "hook", "unhook all");
  int r8 = HotFn(5); fprintf(f, "{\"step\":\"unhooked\",\"r\":%d}\n", r8);

  std::atomic<bool> go(false), quit(false);
  std::thread w([&] {
    while (!go) Sleep(1);
    for (int i = 0; i < 200; i++) { CallerOne(10 + i); if (i % 10 == 0) Sleep(0); }
    for (int i = 0; i < 100; i++) { CallerTwo(1000 + i); if (i % 10 == 0) Sleep(0); }
    while (!quit) Sleep(1);
  });
  Sleep(50);
  Record(f, "watch", "watchw " + Hx((uint64_t)&g_target[0]) + " 8 secs=20 stack=8");
  go = true;
  Sleep(400);
  Record(f, "watch", "watchw result");
  Record(f, "watch", "bps");
  Record(f, "watch", "watchw list");
  Record(f, "watch", "watchw stop");
  Record(f, "watch", "bps");
  Record(f, "watch", "watchw " + Hx((uint64_t)&g_target[0]) + " 64");
  Record(f, "watch", "watchw " + Hx((uint64_t)&g_target[0]) + " 32 secs=1 wait");
  Record(f, "watch", "watchw " + Hx((uint64_t)&g_target[0]) + " 8 secs=1");
  Sleep(1300);
  Record(f, "watch", "bps");
  Record(f, "watch", "watchw list");

  fprintf(f, "{\"step\":\"counterthread\"}\n");
  std::atomic<bool> stopc(false);
  std::thread cth([&] { while (!stopc) { InterlockedIncrement(&g_counter[0]); Sleep(2); } });
  std::string sc = "sample c=" + Hx((uint64_t)&g_counter[0]) + ":i32,t=" + Hx((uint64_t)&g_target[0]) + ":i64 50 1 wait name=unit";
  Record(f, "sample", sc);
  Record(f, "memdiff", "memdiff " + Hx((uint64_t)&g_counter[0]) + " 4 1 hz=100 wait");
  Record(f, "memdiff", "memdiff list");
  Record(f, "memdiff", "memdiff 0 16 1 wait");
  Record(f, "memdiff", "memdiff " + Hx((uint64_t)&g_counter[0]) + " 70000 1");
  Record(f, "memdiff", "memdiff " + Hx((uint64_t)&g_counter[0]) + " auto 1");
  {
    std::atomic<bool> raceStop(false); std::vector<std::string> waits; std::string ctr = Hx((uint64_t)&g_counter[0]);
    std::thread churn([&] { while (!raceStop) { Run("memdiff " + ctr + " 4 1 hz=200"); Run("memdiff stop all"); Run("memdiff list"); Run("sample c=" + ctr + ":i32 100 1"); Run("sample stop all"); Sleep(15); } });
    std::thread waiter([&] { for (int i = 0; i < 3; i++) { waits.push_back(Run("memdiff " + ctr + " 4 1 hz=100 wait")); waits.push_back(Run("sample c=" + ctr + ":i32 50 1 wait")); } });
    waiter.join(); raceStop = true; churn.join();
    int good = 0; for (auto& w : waits) if (w.find("changed runs") != std::string::npos || w.find("done:") != std::string::npos) good++;
    fprintf(f, "{\"step\":\"memdiffrace\",\"waits\":%zu,\"good\":%d,\"sample\":%s}\n", waits.size(), good, jo::Q(waits.empty() ? "" : waits.back()).c_str());
  }
  stopc = true; cth.join();
  Record(f, "sample", "sample status");
  Record(f, "sample", "sample show n=3");
  Record(f, "sample", "sample c=zzz:f99 10 1");
  quit = true; w.join();

  uint8_t* page = (uint8_t*)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
  memset(page, 0x90, 64); page[0] = 0xE9; int32_t prel = 0x20; memcpy(page + 1, &prel, 4); page[0x25] = 0xC3;
  fprintf(f, "{\"step\":\"detourpage\",\"addr\":\"0x%llx\"}\n", (unsigned long long)(uintptr_t)page);
  Record(f, "detour", "hook " + Hx((uint64_t)page) + " agg");
  Record(f, "detour", "hook " + Hx((uint64_t)page) + " hw ret 5");
  Record(f, "detour", "hook " + Hx((uint64_t)page) + " hw");
  Record(f, "detour", "bps");
  Record(f, "detour", "bpc all");
  Record(f, "detour", "hook " + Hx((uint64_t)page) + " force");
  Record(f, "detour", "hook " + Hx((uint64_t)page));
  Record(f, "detour", "hooks");
  Record(f, "detour", "unhook all");
  Record(f, "stats", "stats");
  Record(f, "stats", "stats clear");

  std::string logp = PortLogPath();
  fprintf(f, "{\"step\":\"logpath\",\"path\":%s}\n", jo::Q(logp).c_str());
  fclose(f);
}

int main(int argc, char** argv) {
  char dir[MAX_PATH]; GetTempPathA(MAX_PATH, dir);
  std::string ld = std::string(dir) + "crawler_test_" + std::to_string(GetCurrentProcessId());
  CreateDirectoryA(ld.c_str(), nullptr);
  SetEnvironmentVariableA("CRAWLER_LOGDIR", ld.c_str());
  SetEnvironmentVariableA("CRAWLER_PORT", "27999");
  g_testApi.base = (uintptr_t)GetModuleHandleA(nullptr); g_testApi.gameThreadId = GetCurrentThreadId();
  H = &g_testApi;
  TestJsonOut(); TestAgg(); TestChunk(); TestSampler(); TestWalk(); TestDetour(); TestStats(); TestUopt(); TestMemdiff(); TestRetry(); TestPose(); TestContainers(); TestPlugRestore(); TestHwHook(); TestUeScan(); TestScan();
  printf("unit: %d pass, %d fail\n", g_pass, g_fail);
  if (argc > 2 && !strcmp(argv[1], "--scenario")) {
    CoreInit(&g_testApi);
    Scenario(argv[2]);
    CoreShutdown();
    printf("scenario written to %s\n", argv[2]);
  }
  return g_fail ? 1 : 0;
}

