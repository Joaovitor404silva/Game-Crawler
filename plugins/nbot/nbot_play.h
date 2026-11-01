#pragma once
#include <string>
#include <vector>
#include <set>
#include <map>
#include <cstdlib>
#include <cstdio>
#include <cmath>

namespace npl
{
    struct Stage
    {
        std::string quest, stage, kind, goalSid;
        std::vector<std::string> starts, finishes, events;
        double mx = 0, my = 0, mz = 0, gx = 0, gy = 0, gz = 0;
    };

    inline std::vector<std::string> split(const std::string& s, char d)
    {
        std::vector<std::string> r;
        size_t i = 0;
        while (true)
        {
            size_t j = s.find(d, i);
            r.push_back(s.substr(i, j == std::string::npos ? std::string::npos : j - i));
            if (j == std::string::npos) break;
            i = j + 1;
        }
        return r;
    }

    inline std::vector<std::string> list(const std::string& s)
    {
        if (s.empty() || s == "-") return {};
        return split(s, ',');
    }

    inline std::vector<Stage> load_goals(const std::string& text)
    {
        std::vector<Stage> out;
        size_t i = 0;
        while (i < text.size())
        {
            size_t e = text.find('\n', i);
            if (e == std::string::npos) e = text.size();
            std::string ln = text.substr(i, e - i);
            i = e + 1;
            if (!ln.empty() && ln.back() == '\r') ln.pop_back();
            auto f = split(ln, '\t');
            if (f.size() < 13) continue;
            Stage s;
            s.quest = f[0]; s.stage = f[1]; s.starts = list(f[2]); s.finishes = list(f[3]);
            s.mx = atof(f[4].c_str()); s.my = atof(f[5].c_str()); s.mz = atof(f[6].c_str());
            s.kind = f[7]; s.goalSid = f[8];
            s.gx = atof(f[9].c_str()); s.gy = atof(f[10].c_str()); s.gz = atof(f[11].c_str());
            s.events = list(f[12]);
            out.push_back(s);
        }
        return out;
    }

    struct QuestState
    {
        std::set<std::string> active, done;
    };
    typedef std::map<std::string, QuestState> World;

    inline bool any_in(const std::vector<std::string>& v, const std::set<std::string>& s)
    {
        for (auto& x : v) if (s.count(x)) return true;
        return false;
    }

    inline bool stage_finished(const Stage& st, const QuestState& q)
    {
        if (any_in(st.finishes, q.done)) return true;
        if (st.kind == "trigger" && q.done.count(st.goalSid)) return true;
        return false;
    }

    inline bool stage_started(const Stage& st, const QuestState& q)
    {
        return any_in(st.starts, q.done) || any_in(st.starts, q.active);
    }

    struct Pick
    {
        int idx = -1;
        std::string why;
    };

    inline Pick pick_stage(const std::vector<Stage>& stages, const World& w, const std::string& mainQuest, double px, double py)
    {
        Pick p;
        std::vector<int> cand;
        bool anyJournal = false;
        for (size_t k = 0; k < stages.size(); k++)
        {
            const Stage& s = stages[k];
            if (!mainQuest.empty() && s.quest != mainQuest) continue;
            auto it = w.find(s.quest);
            if (it == w.end()) continue;
            if (stage_started(s, it->second)) anyJournal = true;
            if (s.kind == "none") continue;
            if (stage_started(s, it->second) && !stage_finished(s, it->second)) cand.push_back((int)k);
        }
        if (cand.empty() && !anyJournal)
        {
            double bd = 1e18;
            for (size_t k = 0; k < stages.size(); k++)
            {
                const Stage& s = stages[k];
                if (!mainQuest.empty() && s.quest != mainQuest) continue;
                if (s.kind != "trigger") continue;
                auto it = w.find(s.quest);
                if (it == w.end() || !it->second.active.count(s.goalSid)) continue;
                double d = (s.gx - px) * (s.gx - px) + (s.gy - py) * (s.gy - py);
                if (d < bd) { bd = d; p.idx = (int)k; }
            }
            p.why = p.idx >= 0 ? "fallback: nearest active trigger (no journal node in the done set)" : "no started stage and no active trigger";
            return p;
        }
        if (cand.empty()) { p.why = "no started unfinished stage with a goal"; return p; }
        for (int k : cand)
        {
            const Stage& s = stages[k];
            const QuestState& q = w.at(s.quest);
            if (s.kind == "trigger" && q.active.count(s.goalSid)) { p.idx = k; p.why = "started stage, goal trigger active"; return p; }
        }
        p.idx = cand.back();
        p.why = "started stage";
        return p;
    }

    struct Change
    {
        std::string quest, node;
        int state;
    };

    inline std::vector<Change> diff(const World& a, const World& b)
    {
        std::vector<Change> out;
        for (auto& kv : b)
        {
            auto it = a.find(kv.first);
            static const QuestState empty;
            const QuestState& o = it == a.end() ? empty : it->second;
            for (auto& n : kv.second.active) if (!o.active.count(n)) out.push_back({kv.first, n, 1});
            for (auto& n : kv.second.done) if (!o.done.count(n)) out.push_back({kv.first, n, 3});
            for (auto& n : o.active) if (!kv.second.active.count(n) && !kv.second.done.count(n)) out.push_back({kv.first, n, 0});
        }
        return out;
    }

    inline bool ident(const std::string& s)
    {
        if (s.size() < 3) return false;
        for (char c : s)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return false;
        return true;
    }

    inline bool node_of(const std::string& quest, const std::string& node)
    {
        return ident(node) && node.size() > quest.size() + 1 && node.compare(0, quest.size(), quest) == 0 && node[quest.size()] == '_';
    }

    struct P3 { double x, y, z; };

    inline double dist2d(P3 a, P3 b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y)); }

    inline std::vector<P3> step_candidates(P3 p, P3 t, double len)
    {
        std::vector<P3> out;
        double d = dist2d(p, t);
        if (d < 1) return out;
        double base = std::atan2(t.y - p.y, t.x - p.x);
        static const double angs[] = {0, 30, -30, 60, -60, 90, -90};
        for (double L : {len, len * 0.5})
            for (double a : angs)
            {
                double l = (a == 0 && L > d) ? d : L;
                double r = base + a * 3.14159265358979 / 180.0;
                double z = p.z + (t.z - p.z) * (l / d < 1 ? l / d : 1);
                out.push_back({p.x + std::cos(r) * l, p.y + std::sin(r) * l, z});
            }
        return out;
    }

    inline bool step_gain(P3 start, P3 end, P3 aim, double minGain)
    {
        return dist2d(start, aim) - dist2d(end, aim) >= minGain;
    }

    inline bool same_layer(P3 probe, P3 proj, double maxDz)
    {
        return std::fabs(probe.z - proj.z) <= maxDz;
    }

    struct StepProg
    {
        double best = 1e18;
        int stale = 0, steps = 0;
        bool update(double d)
        {
            steps++;
            if (d < best - 500) { best = d; stale = 0; }
            else stale++;
            return stale < 8 && steps < 120;
        }
    };
}
