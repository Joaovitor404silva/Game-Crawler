#pragma once
#include <string>
#include <vector>
#include <map>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>

namespace nsc
{
    struct JVal
    {
        char t = 'z';
        double n = 0;
        std::string s;
        std::string raw;
    };
    typedef std::map<std::string, JVal> JObj;

    inline size_t skip_ws(const std::string& s, size_t i)
    {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
        return i;
    }

    inline bool parse_str(const std::string& s, size_t& i, std::string& out)
    {
        if (i >= s.size() || s[i] != '"') return false;
        i++;
        out.clear();
        while (i < s.size() && s[i] != '"')
        {
            if (s[i] == '\\' && i + 1 < s.size())
            {
                char c = s[i + 1];
                if (c == 'n') out += '\n';
                else if (c == 't') out += '\t';
                else if (c == 'u' && i + 5 < s.size()) { out += '?'; i += 4; }
                else out += c;
                i += 2;
                continue;
            }
            out += s[i++];
        }
        if (i >= s.size()) return false;
        i++;
        return true;
    }

    inline bool skip_val(const std::string& s, size_t& i)
    {
        i = skip_ws(s, i);
        if (i >= s.size()) return false;
        if (s[i] == '"') { std::string x; return parse_str(s, i, x); }
        if (s[i] == '[' || s[i] == '{')
        {
            int d = 0;
            while (i < s.size())
            {
                if (s[i] == '"') { std::string x; if (!parse_str(s, i, x)) return false; continue; }
                if (s[i] == '[' || s[i] == '{') d++;
                else if (s[i] == ']' || s[i] == '}') { d--; if (d == 0) { i++; return true; } }
                i++;
            }
            return false;
        }
        while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']') i++;
        return true;
    }

    inline bool parse_obj(const std::string& s, JObj& o, size_t start = 0)
    {
        o.clear();
        size_t i = skip_ws(s, start);
        if (i >= s.size() || s[i] != '{') return false;
        i++;
        while (true)
        {
            i = skip_ws(s, i);
            if (i < s.size() && s[i] == '}') return true;
            std::string k;
            if (!parse_str(s, i, k)) return false;
            i = skip_ws(s, i);
            if (i >= s.size() || s[i] != ':') return false;
            i = skip_ws(s, i + 1);
            JVal v;
            size_t v0 = i;
            if (i < s.size() && s[i] == '"') { v.t = 's'; if (!parse_str(s, i, v.s)) return false; }
            else if (s.compare(i, 4, "true") == 0) { v.t = 'b'; v.n = 1; i += 4; }
            else if (s.compare(i, 5, "false") == 0) { v.t = 'b'; v.n = 0; i += 5; }
            else if (s.compare(i, 4, "null") == 0) { v.t = 'z'; i += 4; }
            else if (i < s.size() && (s[i] == '[' || s[i] == '{')) { v.t = 'r'; if (!skip_val(s, i)) return false; }
            else { v.t = 'n'; char* e = nullptr; v.n = strtod(s.c_str() + i, &e); if (e == s.c_str() + i) return false; i = (size_t)(e - s.c_str()); }
            v.raw = s.substr(v0, i - v0);
            if (v.t == 's') v.raw = v.s;
            o[k] = v;
            i = skip_ws(s, i);
            if (i < s.size() && s[i] == ',') { i++; continue; }
            if (i < s.size() && s[i] == '}') return true;
            return false;
        }
    }

    struct CRow
    {
        std::string fam, key;
        JObj f;
        bool has(const std::string& k) const { return f.count(k) != 0; }
        double num(const std::string& k, double d = NAN) const
        {
            auto it = f.find(k);
            if (it == f.end()) return d;
            if (it->second.t == 'n' || it->second.t == 'b') return it->second.n;
            if (it->second.t == 's') { char* e = nullptr; double x = strtod(it->second.s.c_str(), &e); return e != it->second.s.c_str() ? x : d; }
            return d;
        }
        std::string str(const std::string& k) const
        {
            auto it = f.find(k);
            if (it == f.end()) return "";
            if (it->second.t == 'b') return it->second.n != 0 ? "true" : "false";
            return it->second.raw;
        }
    };

    struct Census
    {
        bool ok = false;
        std::string role, err;
        double me[3] = {NAN, NAN, NAN};
        struct Peer { int view; double x, y; };
        std::vector<Peer> peers;
        std::vector<CRow> rows;
        const CRow* find(const std::string& fam, const std::string& key) const
        {
            for (auto& r : rows)
                if ((fam.empty() || fam == "*" || r.fam == fam) && r.key == key) return &r;
            return nullptr;
        }
    };

    inline bool num_array(const std::string& s, const char* name, double* out, int n)
    {
        std::string pat = std::string("\"") + name + "\":[";
        size_t a = s.find(pat);
        if (a == std::string::npos) return false;
        const char* p = s.c_str() + a + pat.size();
        for (int i = 0; i < n; i++)
        {
            char* e = nullptr;
            out[i] = strtod(p, &e);
            if (e == p) return false;
            p = e;
            while (*p == ',' || *p == ' ') p++;
        }
        return true;
    }

    inline Census parse_census(const std::string& text)
    {
        Census c;
        size_t nl = text.find('\n');
        std::string head = text.substr(0, nl);
        if (head.find("\"census\":1") == std::string::npos) { c.err = "not a census file"; return c; }
        size_t rp = head.find("\"role\":\"");
        if (rp != std::string::npos) { size_t e = head.find('"', rp + 8); c.role = head.substr(rp + 8, e - rp - 8); }
        num_array(head, "me", c.me, 3);
        size_t pp = head.find("\"peers\":[");
        if (pp != std::string::npos)
        {
            size_t i = pp + 9;
            while (i < head.size() && head[i] == '{')
            {
                JObj o;
                size_t j = i;
                if (!skip_val(head, j)) break;
                if (parse_obj(head.substr(i, j - i), o)) c.peers.push_back({(int)o["view"].n, o["x"].n, o["y"].n});
                i = j;
                if (i < head.size() && head[i] == ',') i++;
            }
        }
        std::string fam;
        size_t i = nl == std::string::npos ? text.size() : nl + 1;
        while (i < text.size())
        {
            size_t e = text.find('\n', i);
            if (e == std::string::npos) e = text.size();
            std::string ln = text.substr(i, e - i);
            i = e + 1;
            size_t k = skip_ws(ln, 0);
            if (k >= ln.size()) continue;
            if (ln[k] == '"')
            {
                size_t q = k;
                std::string name;
                if (parse_str(ln, q, name) && ln.compare(q, 2, ":[") == 0) fam = name;
                if (name == "digest" || name == "wh" || name == "inv") fam.clear();
                continue;
            }
            if (ln[k] == '{' && !fam.empty())
            {
                size_t j = k;
                if (!skip_val(ln, j)) continue;
                CRow r;
                r.fam = fam;
                if (!parse_obj(ln.substr(k, j - k), r.f)) continue;
                r.key = r.f["key"].s;
                c.rows.push_back(std::move(r));
            }
        }
        c.ok = true;
        return c;
    }

    struct Cond
    {
        std::string field, op, val;
    };

    inline bool parse_cond(const std::string& t, Cond& c)
    {
        static const char* ops[] = {"!=", ">=", "<=", "=", "<", ">", "~"};
        for (const char* op : ops)
        {
            size_t p = t.find(op);
            if (p != std::string::npos && p > 0)
            {
                c.field = t.substr(0, p);
                c.op = op;
                c.val = t.substr(p + strlen(op));
                return true;
            }
        }
        return false;
    }

    inline bool cond_ok(const CRow& r, const Cond& c)
    {
        std::string v = c.field == "key" ? r.key : r.str(c.field);
        if (c.field != "key" && !r.has(c.field)) return false;
        if (c.op == "~") return v.find(c.val) != std::string::npos;
        char* e = nullptr;
        double want = strtod(c.val.c_str(), &e);
        bool wantNum = e != c.val.c_str() && *e == 0;
        double have = c.field == "key" ? NAN : r.num(c.field);
        if (c.val == "true" || c.val == "false")
        {
            bool b = v == "true" || (r.f.count(c.field) && r.f.at(c.field).t == 'n' && r.num(c.field) != 0);
            bool w = c.val == "true";
            if (c.op == "=") return b == w;
            if (c.op == "!=") return b != w;
            return false;
        }
        if (wantNum && !std::isnan(have))
        {
            if (c.op == "=") return std::fabs(have - want) < 1e-6;
            if (c.op == "!=") return std::fabs(have - want) >= 1e-6;
            if (c.op == ">=") return have >= want;
            if (c.op == "<=") return have <= want;
            if (c.op == ">") return have > want;
            if (c.op == "<") return have < want;
        }
        if (c.op == "=") return v == c.val;
        if (c.op == "!=") return v != c.val;
        return false;
    }

    struct Query
    {
        std::string fam, key;
        std::vector<Cond> conds;
        int minCount = 1;
        std::string err;
    };

    inline Query parse_query(const std::vector<std::string>& t, size_t from)
    {
        Query q;
        if (t.size() < from + 2) { q.err = "census <fam> <key|*> [field<op>value ...] [count>=N]"; return q; }
        q.fam = t[from];
        q.key = t[from + 1];
        for (size_t i = from + 2; i < t.size(); i++)
        {
            if (t[i].rfind("timeout=", 0) == 0 || t[i].rfind("as=", 0) == 0) continue;
            if (t[i].rfind("count>=", 0) == 0) { q.minCount = atoi(t[i].c_str() + 7); continue; }
            Cond c;
            if (!parse_cond(t[i], c)) { q.err = "bad condition " + t[i]; return q; }
            q.conds.push_back(c);
        }
        return q;
    }

    inline std::vector<const CRow*> run_query(const Census& c, const Query& q)
    {
        std::vector<const CRow*> out;
        for (auto& r : c.rows)
        {
            if (q.fam != "*" && r.fam != q.fam) continue;
            if (q.key != "*" && r.key != q.key) continue;
            bool ok = true;
            for (auto& cd : q.conds)
                if (!cond_ok(r, cd)) { ok = false; break; }
            if (ok) out.push_back(&r);
        }
        return out;
    }

    inline std::vector<const CRow*> hostiles(const Census& c, double radius, const std::vector<std::string>& allow)
    {
        std::vector<const CRow*> out;
        for (auto& r : c.rows)
        {
            if (r.fam != "npc") continue;
            if (r.str("player") == "true" || r.str("nola") == "true") continue;
            if (r.str("alive") != "true") continue;
            if (r.str("ai") == "none") continue;
            double d = r.num("dist", -1);
            if (d < 0 || d > radius) continue;
            std::string cls = r.str("cls");
            bool ok = false;
            for (auto& a : allow)
                if (!a.empty() && cls.find(a) != std::string::npos) ok = true;
            if (!ok) out.push_back(&r);
        }
        return out;
    }

    inline std::vector<std::string> split_ws(const std::string& s)
    {
        std::vector<std::string> r;
        size_t i = 0;
        while (i < s.size())
        {
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
            size_t j = i;
            while (j < s.size() && !(s[j] == ' ' || s[j] == '\t' || s[j] == '\r' || s[j] == '\n')) j++;
            if (j > i) r.push_back(s.substr(i, j - i));
            i = j;
        }
        return r;
    }

    inline std::string opt(const std::vector<std::string>& t, const char* name, const std::string& d = "")
    {
        std::string p = std::string(name) + "=";
        for (auto& x : t)
            if (x.rfind(p, 0) == 0) return x.substr(p.size());
        return d;
    }

    struct Step
    {
        int line = 0;
        std::string op, text, side;
        std::vector<std::string> t;
        double timeout = 0;
    };

    inline bool parse_hhmm(const std::string& s, int& h, int& m)
    {
        if (sscanf(s.c_str(), "%d:%d", &h, &m) != 2) return false;
        return h >= 0 && h < 24 && m >= 0 && m < 60;
    }

    inline bool parse_rel(const std::string& s, const char* base, double& v)
    {
        std::string b = std::string(base) + "+";
        if (s.rfind(b, 0) != 0) return false;
        char* e = nullptr;
        v = strtod(s.c_str() + b.size(), &e);
        return e != s.c_str() + b.size();
    }

    inline std::string check_step(const Step& s)
    {
        const auto& t = s.t;
        const std::string& o = s.op;
        if (o == "note" || o == "closeui" || o == "hudclean") return "";
        if (o == "require")
        {
            if (t.size() < 3 || t[1] != "nohostile" || atof(t[2].c_str()) <= 0) return "require nohostile <cm> [allow=a,b]";
            return "";
        }
        if (o == "spawn")
        {
            double m;
            if (opt(t, "sid").empty()) return "spawn sid=<SID> at=player+<m> [yaw=<deg>] [freeze=1] [as=<var>]";
            if (!parse_rel(opt(t, "at", "player+3"), "player", m)) return "spawn at=player+<m>";
            return "";
        }
        if (o == "kill") return t.size() >= 2 ? "" : "kill <$var|key>";
        if (o == "faction")
        {
            if (t.size() < 4) return "faction <neutral|friend|enemy|<int>> <FactionA> <FactionB>";
            return "";
        }
        if (o == "give") return t.size() >= 2 ? "" : "give <ItemSID> [n]";
        if (o == "hp") return t.size() >= 2 ? "" : "hp <value>";
        if (o == "god") return t.size() >= 2 ? "" : "god <0|1>";
        if (o == "time") { int h, m; return t.size() >= 2 && parse_hhmm(t[1], h, m) ? "" : "time HH:MM"; }
        if (o == "wait")
        {
            if (t.size() >= 2 && t[1] == "census") { Query q = parse_query(t, 2); return q.err; }
            if (t.size() >= 3 && t[1] == "log") return "";
            if (t.size() >= 2 && atof(t[1].c_str()) > 0) return "";
            return "wait <s> | wait census <fam> <key|*> <cond>... [timeout=s] | wait log <regex> [timeout=s]";
        }
        if (o == "expect")
        {
            if (t.size() >= 2 && t[1] == "census") { Query q = parse_query(t, 2); return q.err; }
            return "expect census <fam> <key|*> <cond>...";
        }
        if (o == "tp")
        {
            if (t.size() < 2) return "tp <x,y,z|player+<m>|peer+<cm>|$var+<cm>>";
            double v;
            double x, y, z;
            if (sscanf(t[1].c_str(), "%lf,%lf,%lf", &x, &y, &z) == 3) return "";
            if (parse_rel(t[1], "player", v) || parse_rel(t[1], "peer", v)) return "";
            if (t[1][0] == '$') return "";
            return "tp target";
        }
        if (o == "frame" || o == "box") return t.size() >= 2 ? "" : o + " <key|$var> ...";
        if (o == "bot") return t.size() >= 2 ? "" : "bot <nbot command>";
        if (o == "key") return t.size() >= 3 && atoi(t[2].c_str()) > 0 ? "" : "key <F|SPACE|..> <ms> (harness side)";
        if (o == "shot") return t.size() >= 2 ? "" : "shot <name> (harness side)";
        return "unknown step " + o;
    }

    inline std::vector<Step> parse_scene(const std::string& text, std::vector<std::string>& errs)
    {
        std::vector<Step> out;
        size_t i = 0;
        int ln = 0;
        while (i <= text.size())
        {
            size_t e = text.find('\n', i);
            if (e == std::string::npos) e = text.size();
            std::string l = text.substr(i, e - i);
            i = e + 1;
            ln++;
            if (!l.empty() && l.back() == '\r') l.pop_back();
            if (ln == 1 && l.size() >= 3 && (unsigned char)l[0] == 0xEF) l = l.substr(3);
            auto t = split_ws(l);
            if (!t.empty() && t[0][0] == '#') t.clear();
            Step s;
            if (!t.empty() && t[0].size() == 2 && t[0][0] == '@')
            {
                s.side = t[0].substr(1);
                t.erase(t.begin());
                size_t a = l.find('@');
                l = l.substr(a + 2);
                while (!l.empty() && (l[0] == ' ' || l[0] == '\t')) l.erase(0, 1);
            }
            if (t.empty()) { if (e >= text.size()) break; continue; }
            s.line = ln;
            s.op = t[0];
            s.t = t;
            s.text = l;
            std::string to = opt(t, "timeout");
            s.timeout = to.empty() ? 20 : atof(to.c_str());
            if (s.op == "scene") { if (e >= text.size()) break; continue; }
            std::string err = check_step(s);
            if (!err.empty()) errs.push_back("line " + std::to_string(ln) + ": " + err + "  (" + l + ")");
            else out.push_back(s);
            if (e >= text.size()) break;
        }
        return out;
    }

    struct Rot { double pitch, yaw; };
    inline Rot look_rot(double fx, double fy, double fz, double tx, double ty, double tz)
    {
        double dx = tx - fx, dy = ty - fy, dz = tz - fz;
        return {atan2(dz, sqrt(dx * dx + dy * dy)) * 57.29577951308232, atan2(dy, dx) * 57.29577951308232};
    }

    inline void stand_point(double tx, double ty, double px, double py, double dist, double& ox, double& oy)
    {
        double dx = px - tx, dy = py - ty, l = sqrt(dx * dx + dy * dy);
        if (l < 1) { dx = 1; dy = 0; l = 1; }
        ox = tx + dx / l * dist;
        oy = ty + dy / l * dist;
    }

    struct Box
    {
        bool any = false;
        int n = 0, inFront = 0;
        double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        double vis = 0;
    };

    inline Box screen_box(const std::vector<std::pair<bool, std::pair<double, double>>>& pts, double w, double h)
    {
        Box b;
        b.n = (int)pts.size();
        bool first = true;
        for (auto& p : pts)
        {
            if (!p.first) continue;
            b.inFront++;
            double x = p.second.first, y = p.second.second;
            if (first) { b.x0 = b.x1 = x; b.y0 = b.y1 = y; first = false; }
            else { b.x0 = (std::min)(b.x0, x); b.x1 = (std::max)(b.x1, x); b.y0 = (std::min)(b.y0, y); b.y1 = (std::max)(b.y1, y); }
        }
        if (first || w <= 0 || h <= 0) return b;
        b.any = true;
        double area = (b.x1 - b.x0) * (b.y1 - b.y0);
        double cx0 = (std::max)(0.0, b.x0), cy0 = (std::max)(0.0, b.y0), cx1 = (std::min)(w, b.x1), cy1 = (std::min)(h, b.y1);
        double carea = (cx1 > cx0 && cy1 > cy0) ? (cx1 - cx0) * (cy1 - cy0) : 0;
        b.vis = area > 0 ? carea / area : 0;
        return b;
    }

    inline void bounds_corners(double ox, double oy, double oz, double ex, double ey, double ez, double out[8][3])
    {
        for (int i = 0; i < 8; i++)
        {
            out[i][0] = ox + ((i & 1) ? ex : -ex);
            out[i][1] = oy + ((i & 2) ? ey : -ey);
            out[i][2] = oz + ((i & 4) ? ez : -ez);
        }
    }

    inline int relation_value(const std::string& s, bool& ok)
    {
        ok = true;
        if (s == "neutral") return 0;
        if (s == "friend") return 1000;
        if (s == "enemy") return -1000;
        char* e = nullptr;
        long v = strtol(s.c_str(), &e, 10);
        ok = e != s.c_str() && *e == 0;
        return (int)v;
    }

    inline std::string jesc(const std::string& v)
    {
        std::string o;
        for (char c : v)
        {
            if (c == '"' || c == '\\') { o += '\\'; o += c; }
            else if (c == '\n') o += "\\n";
            else if (c == '\r') {}
            else if (c == '\t') o += ' ';
            else if ((unsigned char)c < 0x20) o += ' ';
            else o += c;
        }
        return o;
    }

    struct PickCand { bool hidden = false, rendered = true; double dist = 0; };
    inline int pick_visible(const std::vector<PickCand>& c)
    {
        int best = -1;
        double bs = 1e300;
        for (size_t i = 0; i < c.size(); ++i)
        {
            double s = c[i].dist + (c[i].hidden ? 1e9 : 0) + (c[i].rendered ? 0 : 1e6);
            if (s < bs) { bs = s; best = (int)i; }
        }
        return best;
    }
}
