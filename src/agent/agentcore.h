// agentcore: the agent controller as a pure function of state (docs/agent/*-design.md).
// No IO, no threads, no clock: the caller passes State (with its time) and applies Output.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "agentlink.h"

namespace agent {

struct V3 { float x = 0, y = 0, z = 0; };
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Len(V3 a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
struct V2 { float a = 0, b = 0; };
inline float Len(V2 v) { return std::sqrt(v.a * v.a + v.b * v.b); }
inline float Dot(V2 x, V2 y) { return x.a * y.a + x.b * y.b; }
inline V2 Norm(V2 v) { float l = Len(v); return l > 1e-6f ? V2{v.a / l, v.b / l} : V2{}; }
constexpr float kDeg = 57.2957795f;

// Ground plane of a host: up = 1 (Y up) -> (x, z); up = 2 (Z up) -> (x, y).
inline V2 Ground(V3 v, int up) { return up == 1 ? V2{v.x, v.z} : V2{v.x, v.y}; }
inline V3 FromGround(V2 g, int up) { return up == 1 ? V3{g.a, 0, g.b} : V3{g.a, g.b, 0}; }
inline float Height(V3 v, int up) { return up == 1 ? v.y : v.z; }
inline float Wrap180(float d) { while (d > 180) d -= 360; while (d < -180) d += 360; return d; }
inline float Yaw(V2 dir) { return std::atan2(dir.a, dir.b) * kDeg; }

struct Profile {
    int up = 1;
    bool pad = true;           // sticks (pad host) or world move
    float right_sign = 1;      // right of forward (a, b) is right_sign * (b, -a)
    float run_speed = 3.3f;    // m/s at full stick
    float brake = 0.7f;        // overrun after release at run speed, m
    float start_lag_ms = 250;  // stick -> motion
    float slow_radius = 2.5f;  // start slowing down inside this distance
    float min_stick = 0.55f;   // smallest stick that still moves at a useful speed
    float pass_radius = 1.2f;  // intermediate waypoints count as reached inside this
    float level_tol = 2.5f;    // final target: a bigger height difference is another level (goto returns wrong_level)
    float stuck_dist = 0.3f;   // progress below this over stuck_ms = stuck
    float stuck_ms = 1200;
    int max_unstuck = 2;       // unstuck moves before goto returns stuck
    float turn_rate = 125;     // deg/s of camera yaw at rx = 1
    float align_deg = 6;       // face: aligned inside this
    float look_dz = 0;         // right stick deadzone; turn rate = turn_rate * ((|rx| - dz) / (1 - dz)) ^ look_exp
    float look_exp = 1;
    uint32_t dodge = AGL_B_CIRCLE, jump = 0;
};

struct Entity { uint32_t id = 0, kind = 0; V3 pos; float hp_pct = -1; std::string name; };

struct State {
    double t_ms = 0;
    bool has_pos = false, proxy = false; V3 pos, vel;
    bool has_cam = false; V3 cam_pos, cam_fwd;
    bool has_hp = false; float hp = 0, hp_max = 0;
    uint32_t flags = 0; bool has_flags = false;
    uint32_t area = 0; bool has_area = false;
    std::vector<Entity> ents;
};

struct Output {
    float stick[4] = {0, 0, 0, 0};
    uint32_t buttons = 0;
    V3 move; bool has_move = false;
    float look_yaw = 0; bool has_look = false;
};

enum Ev : uint32_t {
    EV_DAMAGED = 1, EV_HP_LOW = 2, EV_ENEMY_NEAR = 4, EV_AREA = 8, EV_MENU = 16, EV_DIED = 32, EV_IDLE = 64,
};
inline const char* EvName(uint32_t e) {
    switch (e) {
    case EV_DAMAGED: return "damaged"; case EV_HP_LOW: return "hp_low"; case EV_ENEMY_NEAR: return "enemy_near";
    case EV_AREA: return "area_changed"; case EV_MENU: return "menu"; case EV_DIED: return "died";
    case EV_IDLE: return "idle"; default: return "?";
    }
}
inline uint32_t EvParse(const std::string& s) {
    static const char* n[] = {"damaged", "hp_low", "enemy_near", "area_changed", "menu", "died", "idle"};
    for (int i = 0; i < 7; ++i) if (s == n[i]) return 1u << i;
    return 0;
}

// Edge-triggered events between two states.
struct EventWatch {
    float hp_low = 0.3f, enemy_r = 8;
    bool primed = false; State prev; uint32_t close_prev = 0;
    uint32_t Step(const State& s) {
        uint32_t ev = 0, close = 0;
        for (auto& e : s.ents)
            if (e.kind == 1 && s.has_pos && Len(e.pos - s.pos) < enemy_r) close = 1;
        if (primed) {
            if (s.has_hp && prev.has_hp && s.hp < prev.hp - 0.01f) ev |= EV_DAMAGED;
            if (s.has_hp && s.hp_max > 0 && s.hp / s.hp_max < hp_low && !(prev.hp_max > 0 && prev.hp / prev.hp_max < hp_low)) ev |= EV_HP_LOW;
            if (close && !close_prev) ev |= EV_ENEMY_NEAR;
            if (s.has_area && prev.has_area && s.area != prev.area) ev |= EV_AREA;
            const uint32_t menu = AGL_F_MENU | AGL_F_DIALOG;
            if (s.has_flags && (s.flags & menu) && !(prev.flags & menu)) ev |= EV_MENU;
            if (s.has_flags && prev.has_flags && !(s.flags & AGL_F_ALIVE) && (prev.flags & AGL_F_ALIVE)) ev |= EV_DIED;
        }
        prev = s; close_prev = close; primed = true;
        return ev;
    }
};

enum class Status { Running, Arrived, Aligned, Stuck, Timeout, Event, NoState, WrongLevel };
inline const char* StatusName(Status s) {
    switch (s) {
    case Status::Running: return "running"; case Status::Arrived: return "arrived"; case Status::Aligned: return "aligned";
    case Status::WrongLevel: return "wrong_level";
    case Status::Stuck: return "stuck"; case Status::Timeout: return "timeout"; case Status::Event: return "event";
    default: return "no_state";
    }
}

// Camera-relative stick (pad hosts) or world move for a ground direction `dir` (unit) at `mag`.
inline void Steer(const Profile& p, const State& s, V2 dir, float mag, Output& o) {
    if (!p.pad) { o.move = FromGround(dir, p.up) * mag; o.has_move = true; return; }
    V2 f = Norm(Ground(s.cam_fwd, p.up));
    if (Len(f) < 0.5f) f = dir; // no camera: push forward toward the target
    V2 r{f.b * p.right_sign, -f.a * p.right_sign};
    o.stick[0] = Dot(dir, r) * mag;
    o.stick[1] = -Dot(dir, f) * mag;
}

// goto: follow waypoints, brake before the last one, climb a ladder of unstuck moves.
class Goto {
public:
    std::vector<V3> pts; float tol = 0.8f; double timeout_ms = 30000; uint32_t stop_on = 0;
    Status status = Status::Running; uint32_t event = 0; float walked = 0; int unstuck_used = 0;
    std::string detail;
    size_t Leg() const { return idx; } // index of the waypoint being walked to (where a stuck happened)

    void Begin(const Profile& p, const State& s) {
        up = p.up; t0 = s.t_ms; idx = 0; last = s.pos; Mark(s);
    }

    Output Step(const Profile& p, const State& s, uint32_t events) {
        Output o;
        if (status != Status::Running) return o;
        if (!s.has_pos || pts.empty()) { status = Status::NoState; return o; }
        walked += Len(Ground(s.pos - last, p.up)); last = s.pos;
        if (events & stop_on) { status = Status::Event; event = events & stop_on; return o; }
        if (s.t_ms - t0 > timeout_ms) { status = Status::Timeout; return o; }
        V2 to = Ground(pts[idx] - s.pos, p.up);
        float d = Len(to);
        const bool final_pt = idx + 1 == pts.size();
        if (!final_pt && d < p.pass_radius) { ++idx; Mark(s); to = Ground(pts[idx] - s.pos, p.up); d = Len(to); }
        if (final_pt) {
            if (d < tol) {
                const float dh = Height(pts[idx] - s.pos, p.up);
                if (std::fabs(dh) > p.level_tol) {
                    status = Status::WrongLevel;
                    char b[128]; std::snprintf(b, sizeof b, "target is %.1f m %s: find stairs, a ramp or a drop", std::fabs(dh), dh > 0 ? "above" : "below");
                    detail = b;
                } else status = Status::Arrived;
                return o;
            }
            // Inside the braking distance at this speed and heading in: release, the overrun carries us.
            const V2 v = Ground(s.vel, p.up);
            const float speed = Len(v);
            const float brake = p.brake * std::fmin(1.f, speed / p.run_speed);
            if (speed > 0.5f && Dot(v, to) > 0 && d < tol + brake) { Mark(s); return o; }
        }
        // Stuck: progress toward the waypoint over the last stuck_ms (rolling) below stuck_dist.
        if (!unstuck_phase) {
            hist.push_back({s.t_ms, d});
            while (hist.size() > 2 && hist[1].first <= s.t_ms - p.stuck_ms) hist.erase(hist.begin());
            if (s.t_ms - mark_ms > p.stuck_ms + p.start_lag_ms && hist.front().first <= s.t_ms - p.stuck_ms &&
                hist.front().second - d < p.stuck_dist) {
                if (unstuck_used >= p.max_unstuck) { status = Status::Stuck; detail = "no progress after unstuck moves"; return o; }
                unstuck_phase = 1; unstuck_t = s.t_ms; ++unstuck_used; side = (unstuck_used & 1) ? 1.f : -1.f;
            }
        }
        V2 dir = Norm(to);
        if (unstuck_phase) {
            const double u = s.t_ms - unstuck_t;
            V2 side_dir{dir.b * side, -dir.a * side};
            if (u < 450) Steer(p, s, V2{-dir.a, -dir.b}, 1.f, o);           // back off
            else if (u < 1250) Steer(p, s, side_dir, 1.f, o);                 // sidestep
            else if (u < 1450) { Steer(p, s, dir, 1.f, o); if (unstuck_used >= 2 && u < 1300) o.buttons |= p.dodge; } // dodge through
            else { unstuck_phase = 0; Mark(s); }
            return o;
        }
        float mag = 1.f;
        if (final_pt && d < p.slow_radius) mag = std::fmax(p.min_stick, d / p.slow_radius);
        Steer(p, s, dir, mag, o);
        return o;
    }

private:
    size_t idx = 0; double t0 = 0, mark_ms = 0, unstuck_t = 0; float mark_d = 0; V3 last;
    int unstuck_phase = 0, up = 1; float side = 1;
    std::vector<std::pair<double, float>> hist; // (t, distance to waypoint) since the last mark
    void Mark(const State& s) {
        mark_ms = s.t_ms; hist.clear();
        mark_d = s.has_pos && !pts.empty() ? Len(Ground(pts[idx] - s.pos, up)) : 0;
    }
};

// face: turn the camera toward a ground yaw (pad: right stick; world hosts: look target).
class Face {
public:
    float yaw = 0; double timeout_ms = 5000; Status status = Status::Running;
    void Begin(const State& s) { t0 = s.t_ms; }
    Output Step(const Profile& p, const State& s) {
        Output o;
        if (status != Status::Running) return o;
        if (!s.has_cam) { status = Status::NoState; return o; }
        float cur = Yaw(Ground(s.cam_fwd, p.up));
        float err = Wrap180(yaw - cur);
        if (std::fabs(err) < p.align_deg) { if (!hold) hold = s.t_ms; if (s.t_ms - hold > 150) status = Status::Aligned; return o; }
        hold = 0;
        if (s.t_ms - t0 > timeout_ms) { status = Status::Timeout; return o; }
        if (!p.pad) { o.look_yaw = yaw; o.has_look = true; return o; }
        // Yaw grows toward +a (atan2(a, b)); right stick right turns toward the right vector.
        // Wanted rate closes the error in ~0.35 s; invert the host's stick curve to get rx.
        float frac = std::fmin(1.f, std::fabs(err) / (p.turn_rate * 0.35f));
        frac = std::fmax(frac, 0.12f);
        float rx = p.look_dz + (1 - p.look_dz) * std::pow(frac, 1.f / p.look_exp);
        o.stick[2] = (err < 0 ? -rx : rx) * p.right_sign;
        return o;
    }
private:
    double t0 = 0, hold = 0;
};

// Compact state text for the LLM (<= ~600 chars).
inline std::string Describe(const Profile& p, const State& s, size_t max_ents = 8) {
    char b[256]; std::string out;
    if (s.has_pos) {
        std::snprintf(b, sizeof b, "pos %.1f %.1f %.1f%s", s.pos.x, s.pos.y, s.pos.z, s.proxy ? " (pivot)" : "");
        out += b;
        float sp = Len(Ground(s.vel, p.up));
        if (sp > 0.2f) { std::snprintf(b, sizeof b, ", moving %.1f m/s", sp); out += b; }
    } else out += "pos unknown";
    V2 f = Norm(Ground(s.cam_fwd, p.up));
    if (s.has_cam) { std::snprintf(b, sizeof b, ", facing %.0f deg", Yaw(f)); out += b; }
    if (s.has_hp) { std::snprintf(b, sizeof b, ", hp %.0f/%.0f", s.hp, s.hp_max); out += b; }
    if (s.has_flags) {
        if (s.flags & AGL_F_LOCKED_ON) out += ", locked on";
        if (s.flags & AGL_F_MENU) out += ", menu open";
        if (s.flags & AGL_F_DIALOG) out += ", dialog";
        if (s.has_flags && !(s.flags & AGL_F_ALIVE)) out += ", dead";
    }
    std::vector<std::pair<float, const Entity*>> close;
    for (auto& e : s.ents) close.push_back({Len(e.pos - s.pos), &e});
    std::sort(close.begin(), close.end(), [](auto& x, auto& y) { return x.first < y.first; });
    for (size_t i = 0; i < close.size() && i < max_ents; ++i) {
        const Entity& e = *close[i].second;
        V2 to = Norm(Ground(e.pos - s.pos, p.up));
        float rel = Wrap180(Yaw(to) - Yaw(f)) * p.right_sign;
        const char* side = std::fabs(rel) < 15 ? "ahead" : std::fabs(rel) > 150 ? "behind" : rel > 0 ? "right" : "left";
        std::snprintf(b, sizeof b, "\n%s#%u %.1f m %s", e.name.empty() ? "entity" : e.name.c_str(), e.id,
                      close[i].first, side);
        out += b;
        if (std::fabs(rel) >= 15 && std::fabs(rel) <= 150) { std::snprintf(b, sizeof b, " %.0f deg", std::fabs(rel)); out += b; }
        if (e.hp_pct >= 0) { std::snprintf(b, sizeof b, " hp %.0f%%", e.hp_pct); out += b; }
    }
    return out;
}

} // namespace agent
