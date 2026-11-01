#pragma once
#include <string>
#include <vector>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

namespace jo {

inline std::string Esc(const std::string& s) {
  std::string r; r.reserve(s.size() + 8);
  for (size_t i = 0; i < s.size(); i++) {
    unsigned char c = (unsigned char)s[i];
    if (c == '"') r += "\\\"";
    else if (c == '\\') r += "\\\\";
    else if (c == '\n') r += "\\n";
    else if (c == '\r') r += "\\r";
    else if (c == '\t') r += "\\t";
    else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); r += b; }
    else if (c < 0x80) r += (char)c;
    else {
      int n = c >= 0xF8 ? 0 : c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC2 ? 1 : 0;
      bool ok = n > 0 && i + (size_t)n < s.size();
      for (int k = 1; ok && k <= n; k++) if (((unsigned char)s[i + k] & 0xC0) != 0x80) ok = false;
      if (ok) { r.append(s, i, n + 1); i += n; } else r += '?';
    }
  }
  return r;
}
inline std::string Q(const std::string& s) { return "\"" + Esc(s) + "\""; }
inline std::string Hex(uint64_t v) { char b[32]; snprintf(b, sizeof b, "\"0x%llx\"", (unsigned long long)v); return b; }
inline std::string Num(long long v) { char b[32]; snprintf(b, sizeof b, "%lld", v); return b; }
inline std::string Dbl(double v) {
  if (v != v || v > 1e300 || v < -1e300) return "null";
  char b[40]; snprintf(b, sizeof b, "%.9g", v); return b;
}

struct Obj {
  std::string s = "{"; bool first = true;
  Obj& raw(const char* k, const std::string& j) { if (!first) s += ","; first = false; s += Q(k) + ":" + j; return *this; }
  Obj& str(const char* k, const std::string& v) { return raw(k, Q(v)); }
  Obj& num(const char* k, long long v) { return raw(k, Num(v)); }
  Obj& dbl(const char* k, double v) { return raw(k, Dbl(v)); }
  Obj& hex(const char* k, uint64_t v) { return raw(k, Hex(v)); }
  Obj& b(const char* k, bool v) { return raw(k, v ? "true" : "false"); }
  std::string done() const { return s + "}"; }
};
inline std::string Arr(const std::vector<std::string>& raw) {
  std::string s = "[";
  for (size_t i = 0; i < raw.size(); i++) { if (i) s += ","; s += raw[i]; }
  return s + "]";
}
inline std::string StrArr(const std::vector<std::string>& v) {
  std::vector<std::string> r; for (auto& x : v) r.push_back(Q(x)); return Arr(r);
}

inline bool StartsWith(const std::string& s, const char* p) { return s.compare(0, strlen(p), p) == 0; }

inline bool LooksLikeError(const std::string& l) {
  static const char* pre[] = { "error", "usage", "cannot ", "unknown command", "no ", "FAILED", "refused", "exception", "already ", "not found",
                               "bad ", "missing", "MH_", "all 4 debug", "slot ", "address must", "arg ", "reflection not", "need ", "scene FAIL",
                               "game thread timeout", "ProcessEvent exception", "names.tsv not loaded", "copy failed", "LoadLibrary failed" };
  for (auto p : pre) if (StartsWith(l, p)) return true;
  return false;
}

inline std::vector<std::string> SplitLines(const std::string& t) {
  std::vector<std::string> r; size_t s = 0, e;
  while ((e = t.find('\n', s)) != std::string::npos) { std::string l = t.substr(s, e - s); if (!l.empty() && l.back() == '\r') l.pop_back(); r.push_back(l); s = e + 1; }
  if (s < t.size()) r.push_back(t.substr(s));
  return r;
}

inline std::string ParseHexBytes(const std::string& line) {
  if (line.size() < 20) return "";
  std::string out;
  for (size_t i = 18; i + 2 <= line.size() && out.size() < 32; i += 3) {
    char a = line[i], b = line[i + 1];
    if (!isxdigit((unsigned char)a) || !isxdigit((unsigned char)b)) break;
    out += a; out += b;
  }
  return out;
}

inline std::string StructureRd(const std::vector<std::string>& lines) {
  if (lines.empty()) return "";
  unsigned long long addr = 0; if (sscanf(lines[0].c_str(), "%llx", &addr) != 1) return "";
  std::string hex;
  for (auto& l : lines) { if (l.find("<unreadable>") != std::string::npos) break; hex += ParseHexBytes(l); }
  return Obj().hex("addr", addr).num("len", (long long)hex.size() / 2).str("hex", hex).b("complete", lines.back().find("<unreadable>") == std::string::npos).done();
}

inline std::string StructurePtr(const std::vector<std::string>& lines) {
  if (lines.empty()) return "";
  const std::string& l = lines[0];
  unsigned long long a = 0, q = 0; float f = 0; double d = 0; int i = 0;
  size_t lb = l.find('['), rb = l.find("]  ->");
  if (lb == std::string::npos || rb == std::string::npos) return "";
  if (sscanf(l.c_str(), "0x%llx", &a) != 1) return "";
  if (sscanf(l.c_str() + rb, "]  -> qword 0x%llx float %g double %lg int %d", &q, &f, &d, &i) != 4) return "";
  Obj o; o.hex("addr", a).str("sym", l.substr(lb + 1, rb - lb - 1)).hex("qword", q).dbl("float", f).dbl("double", d).num("int", i);
  for (size_t k = 1; k < lines.size(); k++) if (StartsWith(lines[k], "  obj: ")) o.str("obj", lines[k].substr(7));
  return o.done();
}

inline std::string StructureSym(const std::vector<std::string>& lines) {
  if (lines.empty()) return "";
  unsigned long long r = 0; char buf[512];
  if (sscanf(lines[0].c_str(), "rva 0x%llx (%511[^)]", &r, buf) == 2) return Obj().hex("rva", r).str("sym", buf).done();
  return StructurePtr(lines);
}

inline std::string StructureFind(const std::vector<std::string>& lines) {
  std::vector<std::string> rows; long long total = -1;
  for (auto& l : lines) {
    unsigned long long a; char cls[256], rest[1024];
    if (l.size() > 2 && l[0] == '0' && l[1] == 'x' && sscanf(l.c_str(), "0x%llx %255s %1023[^\n]", &a, cls, rest) == 3)
      rows.push_back(Obj().hex("addr", a).str("class", cls).str("path", rest).done());
    else if (sscanf(l.c_str(), "%lld hits", &total) == 1) {}
  }
  return Obj().raw("objects", Arr(rows)).num("total", total < 0 ? (long long)rows.size() : total).done();
}

inline std::string StructureGet(const std::vector<std::string>& lines) {
  if (lines.empty()) return "";
  const std::string& l = lines[0];
  size_t at = l.find(" @0x"), eq = l.find(" = ");
  if (at == std::string::npos || eq == std::string::npos || eq < at) return "";
  size_t sp = l.find(' ');
  unsigned long long a = 0; if (sscanf(l.c_str() + at, " @0x%llx", &a) != 1) return "";
  return Obj().str("type", l.substr(0, sp)).str("path", l.substr(sp + 1, at - sp - 1)).hex("addr", a).str("value", l.substr(eq + 3)).done();
}

inline std::string StructureHooks(const std::vector<std::string>& lines) {
  std::vector<std::string> rows;
  for (auto& l : lines) {
    int id; unsigned long long a; long long hits; char mode[64] = ""; char label[256] = "";
    size_t hp = l.find(" hits=");
    if (hp == std::string::npos) continue;
    if (sscanf(l.c_str(), "%d  0x%llx  %255[^\n]", &id, &a, label) < 3) continue;
    std::string lab = label; size_t h2 = lab.find(" hits="); if (h2 != std::string::npos) lab = lab.substr(0, h2);
    while (!lab.empty() && lab.back() == ' ') lab.pop_back();
    if (sscanf(l.c_str() + hp, " hits=%lld mode=%63[^\n]", &hits, mode) < 1) continue;
    std::string md = mode; Obj ro; std::string owner;
    size_t dp = l.find(" detoured_by="); if (dp != std::string::npos) { owner = l.substr(dp + 13); size_t sp = owner.find(' '); if (sp != std::string::npos) owner = owner.substr(0, sp); size_t mp = md.find(" detoured_by="); if (mp != std::string::npos) md = md.substr(0, mp); }
    size_t ag = md.find(" agg"); long long gr = 0, fl = 0;
    if (ag != std::string::npos) {
      size_t gp = md.find("groups="), fp = md.find("filtered=");
      if (gp != std::string::npos) gr = atoll(md.c_str() + gp + 7);
      if (fp != std::string::npos) fl = atoll(md.c_str() + fp + 9);
      md = md.substr(0, ag);
    }
    ro.num("id", id).hex("target", a).str("label", lab).num("hits", hits).str("mode", md).b("agg", ag != std::string::npos);
    if (ag != std::string::npos) ro.num("groups", gr).num("filtered", fl);
    if (!owner.empty()) ro.str("detoured_by", owner);
    rows.push_back(ro.done());
  }
  return Obj().raw("hooks", Arr(rows)).done();
}

inline std::string StructureBps(const std::vector<std::string>& lines) {
  std::vector<std::string> rows;
  for (auto& l : lines) {
    int slot, len; char kind[16]; unsigned long long a; long long hits;
    if (sscanf(l.c_str(), "bp%d %15s 0x%llx len %d hits=%lld", &slot, kind, &a, &len, &hits) == 5)
      rows.push_back(Obj().num("slot", slot).str("kind", kind).hex("addr", a).num("len", len).num("hits", hits).done());
  }
  return Obj().raw("bps", Arr(rows)).done();
}

inline std::string StructureVer(const std::vector<std::string>& lines) {
  if (lines.empty()) return "";
  Obj o; const std::string& l = lines[0];
  size_t p = l.find("built ");
  if (p == std::string::npos) return "";
  char date[32] = "", tm[32] = ""; long long ticks = 0;
  sscanf(l.c_str() + p, "built %31s %*d %*d %31s", date, tm);
  size_t b = l.find("built ") + 6, e = l.find(" ticks=");
  if (e != std::string::npos) { o.str("built", l.substr(b, e - b)); sscanf(l.c_str() + e, " ticks=%lld", &ticks); o.num("ticks", ticks); }
  size_t m = l.find(" md5="); if (m != std::string::npos) o.str("core_md5", l.substr(m + 5, 32));
  size_t lm = l.find(" loader_md5="); if (lm != std::string::npos) { std::string v = l.substr(lm + 12); size_t sp = v.find(' '); o.str("loader_md5", sp == std::string::npos ? v : v.substr(0, sp)); }
  size_t pp = l.find(" port="); if (pp != std::string::npos) o.num("port", atoi(l.c_str() + pp + 6));
  return o.done();
}

inline std::string Structure(const std::string& cmd, const std::vector<std::string>& lines) {
  if (cmd == "rd") return StructureRd(lines);
  if (cmd == "ptr") return StructurePtr(lines);
  if (cmd == "sym" || cmd == "rva") return StructureSym(lines);
  if (cmd == "find") return StructureFind(lines);
  if (cmd == "get") return StructureGet(lines);
  if (cmd == "hooks") return StructureHooks(lines);
  if (cmd == "bps") return StructureBps(lines);
  if (cmd == "ver") return StructureVer(lines);
  return "";
}

inline std::string Envelope(const std::string& cmd, bool ok, const std::vector<std::string>& lines, const std::string& data, int port, long long ms) {
  Obj o; o.b("ok", ok).str("cmd", cmd).num("port", port).num("ms", ms);
  if (!data.empty()) o.raw("data", data);
  o.raw("lines", StrArr(lines));
  return o.done();
}

}
