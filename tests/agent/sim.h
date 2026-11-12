// Toy world for agentcore tests: a pawn on a plane with walls, start lag, accel/brake like Bloodborne
// (run 3.3 m/s, ~0.7 m overrun), a spring camera that follows the heading, wandering entities.
#pragma once
#include "agentcore.h"
#include "walkgraph.h"

namespace sim {
using namespace agent;

struct Rect { float a0, b0, a1, b1; bool In(V2 p, float r = 0.3f) const { return p.a > a0 - r && p.a < a1 + r && p.b > b0 - r && p.b < b1 + r; } };

struct World {
    int up = 1;          // 1: Y up, ground (x, z); 2: Z up, ground (x, y)
    bool pad = true;     // pad host (sticks, camera relative) or world move
    float right_sign = 1;
    V2 pos{0, 0}, vel{0, 0};
    float cam_yaw = 0;   // degrees, atan2(a, b)
    float run = 3.3f, accel = 12, decel = 7.8f, lag_ms = 200;
    double t_ms = 0, push_since = -1;
    std::vector<Rect> walls;
    std::vector<Entity> ents;
    float hp = 100, noise = 0;
    uint32_t rng = 7;
    float Rand(float r) { rng = rng * 1664525u + 1013904223u; return ((rng >> 8) / 16777216.f * 2 - 1) * r; }

    V2 CamFwd() const { float r = cam_yaw / kDeg; return {std::sin(r), std::cos(r)}; }

    void Step(const Output& o, float dt_ms) {
        const float dt = dt_ms / 1000.f;
        V2 want{0, 0};
        if (pad) {
            V2 f = CamFwd(), r{f.b * right_sign, -f.a * right_sign};
            float lx = o.stick[0], ly = -o.stick[1];
            want = {r.a * lx + f.a * ly, r.b * lx + f.b * ly};
            cam_yaw = Wrap180(cam_yaw + o.stick[2] * right_sign * 125.f * dt);
        } else if (o.has_move) {
            want = Ground(o.move, up);
        }
        float mag = Len(want);
        if (mag > 1) { want = {want.a / mag, want.b / mag}; mag = 1; }
        float speed_cap = mag < 0.5f ? mag * 2.4f : run * mag; // walk below half stick
        if (mag > 0.05f) { if (push_since < 0) push_since = t_ms; } else push_since = -1;
        V2 target{0, 0};
        if (push_since >= 0 && t_ms - push_since >= lag_ms && mag > 0) target = {want.a / mag * speed_cap, want.b / mag * speed_cap};
        V2 dv{target.a - vel.a, target.b - vel.b};
        float lim = (Len(target) < Len(vel) ? decel : accel) * dt, l = Len(dv);
        if (l > lim) dv = {dv.a / l * lim, dv.b / l * lim};
        vel = {vel.a + dv.a, vel.b + dv.b};
        V2 next{pos.a + vel.a * dt, pos.b};
        if (Blocked(next)) { next.a = pos.a; vel.a = 0; }
        next.b = pos.b + vel.b * dt;
        if (Blocked(next)) { next.b = pos.b; vel.b = 0; }
        pos = next;
        // spring camera drifts toward the walking direction
        if (Len(vel) > 0.5f) cam_yaw += Wrap180(Yaw(vel) - cam_yaw) * std::fmin(1.f, 0.6f * dt);
        t_ms += dt_ms;
    }
    bool Blocked(V2 p) const { for (auto& w : walls) if (w.In(p)) return true; return false; }

    State Observe() {
        State s; s.t_ms = t_ms;
        V2 p{pos.a + Rand(noise), pos.b + Rand(noise)};
        s.has_pos = true; s.pos = FromGround(p, up); s.vel = FromGround(vel, up);
        s.has_cam = true; s.cam_fwd = FromGround(CamFwd(), up);
        s.has_hp = true; s.hp = hp; s.hp_max = 100;
        s.has_flags = true; s.flags = AGL_F_ALIVE;
        s.ents = ents;
        return s;
    }
};

struct RunResult { Status status; double ms; float final_dist; int flips; };

// Drives a goto to completion at a fixed control rate.
inline RunResult RunGoto(World& w, Profile p, std::vector<V3> pts, float tol, float ctrl_ms = 16, double max_ms = 60000,
                         WalkGraph* rec = nullptr) {
    Goto g; g.pts = pts; g.tol = tol; g.timeout_ms = max_ms;
    State s = w.Observe(); g.Begin(p, s);
    Output o; int flips = 0; float last_lx = 0; double next = 0;
    while (g.status == Status::Running && w.t_ms < max_ms + 1000) {
        if (w.t_ms >= next) {
            s = w.Observe(); o = g.Step(p, s, 0); next = w.t_ms + ctrl_ms;
            if (rec) rec->Add(s.pos, s.t_ms, w.up);
            if (o.stick[0] * last_lx < -0.25f) ++flips;
            if (std::fabs(o.stick[0]) > 0.5f) last_lx = o.stick[0];
        }
        w.Step(o, 4);
    }
    const double t_end = w.t_ms;
    for (int i = 0; i < 150; ++i) w.Step(Output{}, 4); // settle after release
    V2 end = Ground(pts.back(), w.up);
    return {g.status, t_end, Len(V2{end.a - w.pos.a, end.b - w.pos.b}), flips};
}
} // namespace sim
