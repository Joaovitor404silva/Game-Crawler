// Learned walk graph: nodes on ground the player really walked, edges between nodes walked one after the other.
// goto plans through it (A*) so a target behind a wall or on another floor is reached by a known route.
// Pure (no Windows): fed with player positions, tested in tests/agent/test_agent.cpp.
#pragma once
#include "agentcore.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <queue>
#include <string>
#include <vector>

namespace agent {

struct WalkGraph {
    float snap = 0.9f;      // ground m: a sample this close to a node is on that node (a pawn keeps ~0.4 m off walls,
                            // so only walls thinner than ~0.1 m could join both sides)
    float snap_h = 1.0f;    // height m for the same
    float spacing = 1.5f;   // a new node every this many ground metres of track
    float jump = 4.0f;      // longer moves between samples (teleport, respawn) make no edge
    float back_h = 1.2f;    // an edge is walkable backwards when its rise is <= this and <= its ground length (45 deg);
                            // steeper = a drop, one way
    double gap_ms = 1000;   // a pause in samples breaks the track
    std::vector<V3> nodes;
    std::vector<std::vector<int>> out;
    bool dirty = false;

    size_t Edges() const { size_t n = 0; for (auto& o : out) n += o.size(); return n; }

    int Nearest(V3 p, int up, float max_g, float max_h) const {
        int best = -1; float bd = 1e30f;
        for (size_t i = 0; i < nodes.size(); ++i) {
            const V3 d = nodes[i] - p;
            const float g = Len(Ground(d, up));
            if (g > max_g || std::fabs(Height(d, up)) > max_h) continue;
            if (g < bd) { bd = g; best = (int)i; }
        }
        return best;
    }

    void Add(V3 p, double t_ms, int up) {
        if (std::fabs(t_ms - last_t) > gap_ms) last = -1;
        last_t = t_ms;
        int n = Nearest(p, up, snap, snap_h);
        if (n < 0) {
            if (last >= 0) {
                const V3 d = p - nodes[last];
                if (Len(Ground(d, up)) < spacing && std::fabs(Height(d, up)) < snap_h) return; // still near the last node
            }
            nodes.push_back(p); out.emplace_back(); n = (int)nodes.size() - 1; dirty = true;
        }
        if (last >= 0 && n != last) {
            const V3 d = nodes[n] - nodes[last];
            const float g = Len(Ground(d, up)), h = std::fabs(Height(d, up));
            if (g + h <= jump) {
                Link(last, n);
                if (h <= back_h && h <= g) Link(n, last);
            }
        }
        last = n;
    }
    void Break() { last = -1; }

    // A planned goto got stuck at `at` walking the leg a -> b (a door closed, a cart moved): drop the edges along that
    // leg near the stuck spot (both ends within 1.2 m of the leg, one within 3 m of the spot). Walking there again
    // relearns them. Returns how many. Parallel tracks are never joined on purpose: a fence between two walked
    // tracks would join along its whole length.
    int Forget(V3 a, V3 b, V3 at, int up) {
        int n = 0;
        for (size_t u = 0; u < out.size(); ++u)
            for (size_t k = 0; k < out[u].size();) {
                const int v = out[u][k];
                const bool on = OffLine(nodes[u], a, b, up) <= 1.2f && OffLine(nodes[v], a, b, up) <= 1.2f &&
                                (Len(Ground(nodes[u] - at, up)) <= 3 || Len(Ground(nodes[v] - at, up)) <= 3);
                if (on) { out[u].erase(out[u].begin() + k); ++n; dirty = true; }
                else ++k;
            }
        return n;
    }

    // Waypoints from `from` to `to` over known ground, ending with `to` itself. Empty when either end is off the graph
    // (start: `reach` m / `level` m height; target: `reach_to` / `level_to`, looser because targets such as enemies
    // stand off the walked track and their height is not the player's pivot height) or no walked route joins them.
    std::vector<V3> Plan(V3 from, V3 to, int up, float reach = 3.0f, float level = 1.5f, float reach_to = 5.0f,
                         float level_to = 2.5f) const {
        // Every node in reach of either end is a candidate (the nearest one can sit on a track that does not connect,
        // e.g. the same street logged at another pivot height): Dijkstra from all start nodes, entry cost = distance,
        // then the end node with the least route + exit distance.
        std::vector<float> g(nodes.size(), 1e30f);
        std::vector<int> prev(nodes.size(), -1);
        using Q = std::pair<float, int>;
        std::priority_queue<Q, std::vector<Q>, std::greater<Q>> q;
        for (size_t i = 0; i < nodes.size(); ++i)
            if (In(nodes[i] - from, up, reach, level)) { g[i] = Len(nodes[i] - from); q.push({g[i], (int)i}); }
        while (!q.empty()) {
            const auto [c0, u] = q.top(); q.pop();
            if (c0 > g[u]) continue;
            for (int v : out[u]) {
                const float c = g[u] + Len(nodes[v] - nodes[u]);
                if (c < g[v]) { g[v] = c; prev[v] = u; q.push({c, v}); }
            }
        }
        int b = -1; float best = 1e30f;
        for (size_t i = 0; i < nodes.size(); ++i)
            if (g[i] < 1e29f && In(nodes[i] - to, up, reach_to, level_to) && g[i] + Len(nodes[i] - to) < best) { best = g[i] + Len(nodes[i] - to); b = (int)i; }
        if (b < 0) return {};
        std::vector<V3> path;
        for (int v = b; v >= 0; v = prev[v]) path.push_back(nodes[v]);
        std::reverse(path.begin(), path.end());
        std::vector<V3> pts = Thin(path, up);
        pts.push_back(to);
        return pts;
    }

    // Drops nodes that lie within 0.5 m of the straight line between their kept neighbours (straight runs of track).
    static std::vector<V3> Thin(const std::vector<V3>& p, int up) {
        if (p.size() < 3) return p;
        std::vector<V3> r{p[0]};
        size_t anchor = 0;
        for (size_t j = 2; j < p.size(); ++j) {
            bool ok = true;
            for (size_t k = anchor + 1; k < j && ok; ++k) ok = OffLine(p[k], p[anchor], p[j], up) <= 0.5f;
            if (!ok) { r.push_back(p[j - 1]); anchor = j - 1; }
        }
        r.push_back(p.back());
        return r;
    }

    bool Save(const std::string& path) {
        std::ofstream f(path);
        if (!f) return false;
        for (auto& n : nodes) f << "v " << n.x << " " << n.y << " " << n.z << "\n";
        for (size_t i = 0; i < out.size(); ++i) for (int v : out[i]) f << "e " << i << " " << v << "\n";
        dirty = false;
        return true;
    }
    bool Load(const std::string& path) {
        std::ifstream f(path);
        if (!f) return false;
        nodes.clear(); out.clear(); last = -1;
        std::string k;
        while (f >> k) {
            if (k == "v") { V3 v; f >> v.x >> v.y >> v.z; nodes.push_back(v); out.emplace_back(); }
            else if (k == "e") { size_t a, b; f >> a >> b; if (a < out.size() && b < out.size()) Link((int)a, (int)b); }
        }
        dirty = false;
        return true;
    }

private:
    int last = -1; double last_t = -1e18;
    static bool In(V3 d, int up, float max_g, float max_h) { return Len(Ground(d, up)) <= max_g && std::fabs(Height(d, up)) <= max_h; }
    void Link(int a, int b) { if (std::find(out[a].begin(), out[a].end(), b) == out[a].end()) { out[a].push_back(b); dirty = true; } }
    static float OffLine(V3 p, V3 a, V3 b, int up) {
        const V2 ab = Ground(b - a, up), ap = Ground(p - a, up);
        const float l2 = ab.a * ab.a + ab.b * ab.b;
        const float t = l2 > 1e-6f ? std::fmax(0.f, std::fmin(1.f, (ap.a * ab.a + ap.b * ab.b) / l2)) : 0.f;
        const V2 d{ap.a - ab.a * t, ap.b - ab.b * t};
        const float hd = std::fabs(Height(p - a, up) - Height(b - a, up) * t);
        return std::fmax(Len(d), hd);
    }
};

} // namespace agent
