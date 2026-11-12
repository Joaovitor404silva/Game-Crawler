// agentd: runs agentcore against a game's AgentLink and answers line commands on 127.0.0.1:<port>.
// One command at a time; each reply ends with ".\n" (same framing as the crawler). See docs/agent.
//   agentd [--pid N] [--port 27070] [--profile file.ini] [--dir agent_data_dir]
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <tlhelp32.h>
#include <mmsystem.h>
#include "agentcore.h"
#include "walkgraph.h"
#include "look.h"
#include "wgc.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <mutex>
#include <thread>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")

using namespace agent;

static AglLink* g_link;
static HANDLE g_proc;
static uint32_t g_pid;
static Profile g_prof;
static std::string g_dir, g_game, g_prof_path;
static std::map<std::string, V3> g_marks;
static FILE* g_flight;
static EventWatch g_events;
static uint32_t g_ev_pending; // events seen since the last command returned
static std::mutex g_cmd;      // one command at a time across client threads
static WalkGraph g_graph;      // learned from every walked position; goto plans through it

// ---------------------------------------------------------------- link
static uint32_t FindHostPid() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe{sizeof(pe)};
    uint32_t found = 0;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe)) {
        wchar_t name[64]; swprintf(name, 64, L"Local\\AgentLink_%u", pe.th32ProcessID);
        HANDLE m = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
        if (m) { found = pe.th32ProcessID; CloseHandle(m); }
    }
    CloseHandle(snap);
    return found;
}

static bool Alive() { return g_proc && WaitForSingleObject(g_proc, 0) == WAIT_TIMEOUT; }

static State ReadState() {
    State s; s.t_ms = agl_now_ms();
    AglPlayer p; AglCamera c;
    if (agl_read(&g_link->player, sizeof p, &p)) {
        s.has_pos = p.valid & AGL_V_POS; s.proxy = p.valid & AGL_V_PROXY;
        s.pos = {p.pos[0], p.pos[1], p.pos[2]};
        if (p.valid & AGL_V_VEL) s.vel = {p.vel[0], p.vel[1], p.vel[2]};
        s.has_hp = p.valid & AGL_V_HP; s.hp = p.hp; s.hp_max = p.hp_max;
        s.has_flags = p.valid & AGL_V_FLAGS; s.flags = p.flags;
        s.has_area = p.valid & AGL_V_AREA; s.area = p.area_id;
    }
    if (agl_read(&g_link->camera, sizeof c, &c) && c.valid) {
        s.has_cam = true; s.cam_pos = {c.pos[0], c.pos[1], c.pos[2]}; s.cam_fwd = {c.fwd[0], c.fwd[1], c.fwd[2]};
    }
    static AglEntities e; // 3 KiB: keep off the stack
    if (agl_read(&g_link->ents, sizeof e, &e))
        for (uint32_t i = 0; i < e.count && i < AGL_MAX_ENTITIES; ++i) {
            Entity x; x.id = e.e[i].id; x.kind = e.e[i].kind; x.hp_pct = e.e[i].hp_pct;
            x.pos = {e.e[i].pos[0], e.e[i].pos[1], e.e[i].pos[2]}; x.name.assign(e.e[i].name, strnlen(e.e[i].name, 24));
            s.ents.push_back(x);
        }
    return s;
}

static void Send(const Output& o, uint32_t ttl = 200) {
    AglCommand& c = g_link->cmd;
    agl_begin(&c.seq);
    c.cmd_seq++;
    c.ttl_ms = ttl; c.buttons = o.buttons;
    for (int i = 0; i < 4; ++i) c.stick[i] = o.stick[i];
    c.move[0] = o.move.x; c.move[1] = o.move.y; c.move[2] = o.move.z;
    c.look[0] = o.look_yaw; c.flags = o.has_look ? 2u : 0u;
    c.issued_ms = agl_now_ms();
    agl_end(&c.seq);
}
static void Release() { Send(Output{}, 0); }

static void Flight(const State& s, const Output& o, const char* task) {
    static double last;
    if (!g_flight || s.t_ms - last < 100) return;
    last = s.t_ms;
    std::fprintf(g_flight, "{\"t\":%.0f,\"task\":\"%s\",\"pos\":[%.2f,%.2f,%.2f],\"yaw\":%.1f,\"stick\":[%.2f,%.2f,%.2f,%.2f],\"btn\":%u,\"hp\":%.1f}\n",
                 s.t_ms, task, s.pos.x, s.pos.y, s.pos.z, Yaw(Norm(Ground(s.cam_fwd, g_prof.up))), o.stick[0], o.stick[1],
                 o.stick[2], o.stick[3], o.buttons, s.hp);
    std::fflush(g_flight);
}

// Polls the link until `step` returns false. step gets each new state (player or camera frame changed), and at
// least every 20 ms so timed steps end on screens that publish no player/camera frames (menus, loading).
template <class F> static void Loop(const char* task, F step) {
    uint64_t last_p = ~0ull, last_c = ~0ull;
    double last_t = 0;
    Output o;
    for (;;) {
        if (!Alive()) { Release(); return; }
        const uint64_t fp = g_link->player.frame_id, fc = g_link->camera.frame_id;
        if (fp != last_p || fc != last_c || agl_now_ms() - last_t >= 20) {
            last_p = fp; last_c = fc; last_t = agl_now_ms();
            State s = ReadState();
            g_ev_pending |= g_events.Step(s);
            if (s.has_pos && !s.proxy) g_graph.Add(s.pos, s.t_ms, g_prof.up);
            if (!step(s, o)) { Release(); return; }
            Flight(s, o, task);
        }
        Send(o);
        Sleep(2);
    }
}

// ---------------------------------------------------------------- profile, marks
static void LoadProfile(const std::string& path) {
    std::ifstream f(path); std::string line;
    std::map<std::string, float*> fl = {
        {"run_speed", &g_prof.run_speed}, {"brake", &g_prof.brake}, {"start_lag_ms", &g_prof.start_lag_ms},
        {"slow_radius", &g_prof.slow_radius}, {"min_stick", &g_prof.min_stick}, {"pass_radius", &g_prof.pass_radius}, {"level_tol", &g_prof.level_tol},
        {"stuck_dist", &g_prof.stuck_dist}, {"stuck_ms", &g_prof.stuck_ms}, {"turn_rate", &g_prof.turn_rate},
        {"align_deg", &g_prof.align_deg}, {"right_sign", &g_prof.right_sign},
        {"look_dz", &g_prof.look_dz}, {"look_exp", &g_prof.look_exp},
        {"hp_low", &g_events.hp_low}, {"enemy_r", &g_events.enemy_r}};
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        while (!k.empty() && k.back() == ' ') k.pop_back();
        while (!v.empty() && v.front() == ' ') v.erase(0, 1);
        if (fl.count(k)) *fl[k] = (float)atof(v.c_str());
        else if (k == "max_unstuck") g_prof.max_unstuck = atoi(v.c_str());
        else if (k == "dodge") g_prof.dodge = 1u << atoi(v.c_str());
    }
}
static void SaveProfileKey(const std::string& key, float value) {
    std::ifstream in(g_prof_path); std::string line, out; bool done = false;
    char kv[96]; std::snprintf(kv, sizeof kv, "%s = %.3f", key.c_str(), value);
    while (std::getline(in, line)) {
        auto eq = line.find('=');
        std::string k = eq == std::string::npos ? "" : line.substr(0, eq);
        while (!k.empty() && k.back() == ' ') k.pop_back();
        if (k == key) { out += kv; done = true; } else out += line;
        out += "\n";
    }
    if (!done) { out += kv; out += "\n"; }
    in.close();
    std::ofstream(g_prof_path) << out;
}
static std::string MarksPath() { return g_dir + "\\maps\\" + g_game + ".txt"; }
static void LoadMarks() {
    std::ifstream f(MarksPath()); std::string n; V3 p;
    while (f >> n >> p.x >> p.y >> p.z) g_marks[n] = p;
}
static void SaveMarks() {
    CreateDirectoryA((g_dir + "\\maps").c_str(), nullptr);
    std::ofstream f(MarksPath());
    for (auto& [n, p] : g_marks) f << n << " " << p.x << " " << p.y << " " << p.z << "\n";
}

static std::string GraphPath() { return g_dir + "\\maps\\" + g_game + ".graph"; }
static void SaveGraph() { CreateDirectoryA((g_dir + "\\maps").c_str(), nullptr); g_graph.Save(GraphPath()); }
// No graph file yet: learn it from the positions in this game's old flight logs.
static void ImportFlightLogs() {
    std::vector<std::string> files;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((g_dir + "\\logs\\" + g_game + "-*.jsonl").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do files.push_back(g_dir + "\\logs\\" + fd.cFileName); while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(files.begin(), files.end());
    for (auto& f : files) {
        std::ifstream in(f); std::string line;
        g_graph.Break();
        while (std::getline(in, line)) {
            const size_t t = line.find("\"t\":"), p = line.find("\"pos\":[");
            double tm; V3 v;
            if (t == std::string::npos || p == std::string::npos) continue;
            if (std::sscanf(line.c_str() + t + 4, "%lf", &tm) == 1 && std::sscanf(line.c_str() + p + 7, "%f,%f,%f", &v.x, &v.y, &v.z) == 3)
                g_graph.Add(v, tm, g_prof.up);
        }
    }
}

// ---------------------------------------------------------------- commands
static std::map<std::string, std::string> Opts(std::istringstream& in, std::vector<std::string>& args) {
    std::map<std::string, std::string> o; std::string t;
    while (in >> t) { auto eq = t.find('='); if (eq == std::string::npos) args.push_back(t); else o[t.substr(0, eq)] = t.substr(eq + 1); }
    return o;
}
static bool ParseV3(const std::string& t, V3& v) { return std::sscanf(t.c_str(), "%f,%f,%f", &v.x, &v.y, &v.z) == 3; }
static const char* kButtons[] = {"cross", "circle", "square", "triangle", "l1", "r1", "l2", "r2", "l3", "r3",
                                 "options", "touchpad", "up", "down", "left", "right"};
static uint32_t ParseButtons(const std::string& t) {
    uint32_t b = 0; std::stringstream ss(t); std::string x;
    while (std::getline(ss, x, '+')) for (int i = 0; i < 16; ++i) if (x == kButtons[i]) b |= 1u << i;
    return b;
}
static uint32_t ParseEvents(const std::string& t) {
    uint32_t e = 0; std::stringstream ss(t); std::string x;
    while (std::getline(ss, x, ',')) e |= EvParse(x);
    return e;
}
static std::string EventsText(uint32_t e) {
    std::string s;
    for (int i = 0; i < 7; ++i) if (e & (1u << i)) { if (!s.empty()) s += ","; s += EvName(1u << i); }
    return s;
}
static std::string Fmt(const char* f, ...) {
    char b[512]; va_list a; va_start(a, f); vsnprintf(b, sizeof b, f, a); va_end(a); return b;
}

// Resolves a target: "x,y,z", a mark name, or fwd=/right= relative to the camera.
static bool Target(const std::string& t, const State& s, V3& out) {
    if (ParseV3(t, out)) return true;
    if (t.size() > 1 && t[0] == '#') { // entity id from agent_state ("hp 239#268517413")
        const uint32_t id = (uint32_t)strtoul(t.c_str() + 1, nullptr, 10);
        for (auto& e : s.ents) if (e.id == id) { out = e.pos; return true; }
        return false;
    }
    auto m = g_marks.find(t);
    if (m != g_marks.end()) { out = m->second; return true; }
    return false;
}
static V3 Relative(const State& s, float fwd, float right) {
    V2 f = Norm(Ground(s.cam_fwd, g_prof.up)), r{f.b * g_prof.right_sign, -f.a * g_prof.right_sign};
    V3 d = FromGround(V2{f.a * fwd + r.a * right, f.b * fwd + r.b * right}, g_prof.up);
    return s.pos + d;
}

static std::string CmdGoto(std::istringstream& in) {
    std::vector<std::string> args; auto o = Opts(in, args);
    State s = ReadState();
    if (!s.has_pos) return "error: no player position from the host";
    Goto g;
    for (auto& a : args) {
        std::stringstream ss(a); std::string p;
        while (std::getline(ss, p, ';')) { V3 v; if (!Target(p, s, v)) return "error: unknown target " + p; g.pts.push_back(v); }
    }
    if (o.count("fwd") || o.count("right"))
        g.pts.push_back(Relative(s, o.count("fwd") ? (float)atof(o["fwd"].c_str()) : 0, o.count("right") ? (float)atof(o["right"].c_str()) : 0));
    if (g.pts.empty()) return "error: goto <x,y,z[;x,y,z...]|mark> or fwd=<m> right=<m>";
    // One named/absolute target: walk a learned route when known ground joins here and there (path=0: straight).
    std::string via;
    if (g.pts.size() == 1 && !o.count("fwd") && !o.count("right") && o["path"] != "0") {
        auto plan = g_graph.Plan(s.pos, g.pts[0], g_prof.up, 3.0f, 1.5f, 5.0f, g_prof.level_tol);
        if (plan.size() >= 2) { via = Fmt(", via walk graph (%d waypoints)", (int)plan.size() - 1); g.pts = plan; }
        else via = ", straight (no walked route known)";
    }
    g.tol = o.count("tol") ? (float)atof(o["tol"].c_str()) : 0.8f;
    g.timeout_ms = (o.count("timeout") ? atof(o["timeout"].c_str()) : 30) * 1000;
    g.stop_on = o.count("stop_on") ? ParseEvents(o["stop_on"]) : 0;
    g.Begin(g_prof, s);
    const double t0 = s.t_ms;
    g_ev_pending = 0; // stop_on watches what happens during this goto, not events left from earlier commands

    Loop("goto", [&](const State& st, Output& out) { out = g.Step(g_prof, st, g_ev_pending); g_ev_pending = 0; return g.status == Status::Running; });
    if (!Alive()) return "error: game exited";
    // Report where the pawn comes to rest, not where the stick was released.
    const double t_rel = agl_now_ms();
    const float took = (float)(t_rel - t0) / 1000;
    const V3 released = ReadState().pos;
    Loop("settle", [&](const State& st, Output& out) { out = Output{}; return Len(Ground(st.vel, g_prof.up)) > 0.3f && agl_now_ms() - t_rel < 900; });
    State e = ReadState();
    std::string r = Fmt("%s in %.1f s, walked %.1f m, %.1f m from target (height %+.1f m), %d unstuck move(s)", StatusName(g.status),
                        took, g.walked + Len(Ground(e.pos - released, g_prof.up)), Len(Ground(g.pts.back() - e.pos, g_prof.up)),
                        Height(g.pts.back() - e.pos, g_prof.up), g.unstuck_used);
    if (g.status == Status::Event) r += ", event " + EventsText(g.event);
    if (!g.detail.empty()) r += ", " + g.detail;
    r += via;
    if (g.status == Status::Stuck && via.rfind(", via", 0) == 0) { // the learned route is blocked now (a door, a cart)
        const size_t k = g.Leg();
        const int n = g_graph.Forget(k ? g.pts[k - 1] : s.pos, g.pts[k], e.pos, g_prof.up);
        r += Fmt(", forgot %d walk-graph edge(s) at the stuck spot", n);
    }
    if (g_flight) { // one result row per goto, for stuck-rate stats (tools/agent_stats.py)
        std::fprintf(g_flight, "{\"t\":%.0f,\"result\":\"goto\",\"status\":\"%s\",\"s\":%.1f,\"walked\":%.1f,\"straight\":%.1f,\"left\":%.1f,\"unstuck\":%d,\"graph\":%d}\n",
                     e.t_ms, StatusName(g.status), took, g.walked, Len(Ground(g.pts.back() - s.pos, g_prof.up)),
                     Len(Ground(g.pts.back() - e.pos, g_prof.up)), g.unstuck_used, via.rfind(", via", 0) == 0 ? 1 : 0);
        std::fflush(g_flight);
    }
    return r + "\n" + Describe(g_prof, e);
}

static std::string CmdFace(std::istringstream& in) {
    std::vector<std::string> args; auto o = Opts(in, args);
    State s = ReadState();
    Face f;
    if (o.count("yaw")) f.yaw = (float)atof(o["yaw"].c_str());
    else if (o.count("entity")) {
        uint32_t id = (uint32_t)atoi(o["entity"].c_str()); bool hit = false;
        for (auto& e : s.ents) if (e.id == id) { f.yaw = Yaw(Ground(e.pos - s.pos, g_prof.up)); hit = true; }
        if (!hit) return "error: no entity " + o["entity"];
    } else if (!args.empty()) {
        V3 t; if (!Target(args[0], s, t)) return "error: unknown target " + args[0];
        f.yaw = Yaw(Ground(t - s.pos, g_prof.up));
    } else return "error: face <x,y,z|mark> or yaw=<deg> or entity=<id>";
    f.Begin(s);
    Loop("face", [&](const State& st, Output& out) { out = f.Step(g_prof, st); return f.status == Status::Running; });
    return Fmt("%s, yaw target %.0f\n", StatusName(f.status), f.yaw) + Describe(g_prof, ReadState());
}

// do: "tap circle; hold r1 300; wait 200; stick ly=-1 lx=0.5 800; hold l1+r1 100"
static std::string CmdDo(const std::string& rest) {
    struct Seg { Output o; double ms; };
    std::vector<Seg> segs; std::stringstream ss(rest); std::string part;
    while (std::getline(ss, part, ';')) {
        std::istringstream p(part); std::string verb; p >> verb;
        if (verb.empty()) continue;
        Seg sg{}; sg.ms = 0;
        if (verb == "tap") { std::string b; p >> b; sg.o.buttons = ParseButtons(b); sg.ms = 90; segs.push_back(sg); sg.o = Output{}; sg.ms = 60; }
        else if (verb == "hold") { std::string b; p >> b >> sg.ms; sg.o.buttons = ParseButtons(b); }
        else if (verb == "wait") { p >> sg.ms; }
        else if (verb == "stick") {
            std::string t;
            while (p >> t) {
                const char* ax[] = {"lx=", "ly=", "rx=", "ry="}; bool hit = false;
                for (int i = 0; i < 4; ++i) if (!t.compare(0, 3, ax[i])) { sg.o.stick[i] = (float)atof(t.c_str() + 3); hit = true; }
                if (!hit) sg.ms = atof(t.c_str());
            }
        } else return "error: do verbs: tap <b>, hold <b[+b]> <ms>, wait <ms>, stick lx= ly= rx= ry= <ms>";
        segs.push_back(sg);
    }
    State s0 = ReadState();
    for (auto& sg : segs) {
        const double end = agl_now_ms() + sg.ms;
        Loop("do", [&](const State&, Output& out) { out = sg.o; return agl_now_ms() < end; });
        // Loop only steps on new frames: keep sending until the time is up even between frames
        while (agl_now_ms() < end && Alive()) { Send(sg.o); Sleep(2); }
    }
    Release();
    Sleep(150);
    State s1 = ReadState();
    return Fmt("done, moved %.2f m\n", Len(s1.pos - s0.pos)) + Describe(g_prof, s1);
}

static std::string CmdWait(std::istringstream& in) {
    std::vector<std::string> args; auto o = Opts(in, args);
    uint32_t until = o.count("until") ? ParseEvents(o["until"]) : 0;
    const double end = agl_now_ms() + (o.count("timeout") ? atof(o["timeout"].c_str()) : 10) * 1000;
    uint32_t hit = 0;
    g_ev_pending = 0;
    Loop("wait", [&](const State&, Output& out) { out = Output{}; hit = g_ev_pending & until; g_ev_pending = 0; return !hit && agl_now_ms() < end; });
    return (hit ? "event " + EventsText(hit) : std::string("timeout")) + "\n" + Describe(g_prof, ReadState());
}

static std::string CmdCalibrate() {
    // Needs ~6 m of open ground ahead and to the right.
    auto run = [&](Output o, double ms, std::vector<std::pair<double, State>>& out) {
        const double t0 = agl_now_ms();
        Loop("calibrate", [&](const State& s, Output& c) { c = o; out.push_back({s.t_ms - t0, s}); return s.t_ms - t0 < ms; });
        Release();
        const double t1 = agl_now_ms();
        Loop("calibrate", [&](const State& s, Output& c) { c = Output{}; out.push_back({s.t_ms - t0, s}); return agl_now_ms() - t1 < 900; });
    };
    std::vector<std::pair<double, State>> a, b, c;
    const double t_rest = agl_now_ms(); // start from rest
    Loop("calibrate", [&](const State& s, Output& o) { o = Output{}; return Len(Ground(s.vel, g_prof.up)) > 0.1f && agl_now_ms() - t_rest < 2000; });
    Output fwd;
    if (g_prof.pad) fwd.stick[1] = -1;
    else { // world host: push along the camera's ground forward
        State s = ReadState();
        if (!s.has_cam) return "error: no camera from the host";
        fwd.move = FromGround(Norm(Ground(s.cam_fwd, g_prof.up)), g_prof.up); fwd.has_move = true;
    }
    run(fwd, 1800, a);
    if (a.size() < 10) return "error: no frames";
    V3 p0 = a.front().second.pos; double lag = -1, rel = 0; V3 prel{}; V3 pend = a.back().second.pos;
    for (auto& [t, s] : a) { if (lag < 0 && Len(s.pos - p0) > 0.05f) lag = t; if (t <= 1800) { prel = s.pos; rel = t; } }
    float speed = 0; // speed over the last 600 ms of the push
    for (auto& [t, s] : a) if (t >= 1200 && t <= 1800) { speed = Len(Ground(prel - s.pos, g_prof.up)) / (float)((rel - t) / 1000 + 1e-6); break; }
    float overrun = Len(Ground(pend - prel, g_prof.up));
    if (!g_prof.pad) { // strafe side and turn rate are stick properties: world hosts move and turn exactly
        if (speed > 0.5f) { g_prof.run_speed = speed; SaveProfileKey("run_speed", speed); }
        if (overrun > 0.05f) { g_prof.brake = overrun; SaveProfileKey("brake", overrun); }
        if (lag > 0) { g_prof.start_lag_ms = (float)lag; SaveProfileKey("start_lag_ms", (float)lag); }
        return Fmt("run %.2f m/s, start lag %.0f ms, overrun %.2f m (world host: no stick tests; saved to %s)",
                   speed, lag, overrun, g_prof_path.c_str());
    }
    Output right; right.stick[0] = 1; run(right, 900, b);
    V2 f0 = Norm(Ground(b.front().second.cam_fwd, g_prof.up));
    V2 mv = Ground(b.back().second.pos - b.front().second.pos, g_prof.up);
    float side = f0.b * mv.a - f0.a * mv.b; // dot(move, (b, -a))
    Output turn; turn.stick[2] = 1; run(turn, 600, c);
    float y0 = Yaw(Ground(c.front().second.cam_fwd, g_prof.up)), y1 = 0; double t1 = 0;
    for (auto& [t, s] : c) if (t <= 600) { y1 = Yaw(Ground(s.cam_fwd, g_prof.up)); t1 = t; }
    float rate = std::fabs(Wrap180(y1 - y0)) / (float)(t1 / 1000 + 1e-6);
    if (speed > 0.5f) { g_prof.run_speed = speed; SaveProfileKey("run_speed", speed); }
    if (overrun > 0.05f) { g_prof.brake = overrun; SaveProfileKey("brake", overrun); }
    if (lag > 0) { g_prof.start_lag_ms = (float)lag; SaveProfileKey("start_lag_ms", (float)lag); }
    if (std::fabs(side) > 0.3f) { g_prof.right_sign = side > 0 ? 1.f : -1.f; SaveProfileKey("right_sign", g_prof.right_sign); }
    if (rate > 10) { g_prof.turn_rate = rate; SaveProfileKey("turn_rate", rate); }
    return Fmt("run %.2f m/s, start lag %.0f ms, overrun %.2f m, strafe side %+.2f -> right_sign %+.0f, turn %.0f deg/s (saved to %s)",
               speed, lag, overrun, side, g_prof.right_sign, rate, g_prof_path.c_str());
}

// World point -> 0..1 image coords through the host camera (view rows [R|t], clip = (xs x, ys y, ., z)).
static bool Project(const AglCamera& c, V3 p, float& u, float& v) {
    const float* m = c.view;
    const float vx = m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3];
    const float vy = m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7];
    const float vz = m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11];
    if (vz < 0.2f || c.proj[0] == 0) return false;
    u = 0.5f + 0.5f * c.proj[0] * vx / vz;
    v = 0.5f - 0.5f * c.proj[1] * vy / vz;
    return u > -0.05f && u < 1.05f && v > -0.05f && v < 1.05f;
}

static std::string CmdLook(std::istringstream& in) {
    static look::Grabber grab;
    std::vector<std::string> args; auto o = Opts(in, args);
    const int width = o.count("width") ? atoi(o["width"].c_str()) : 960;
    const bool overlay = !o.count("overlay") || o["overlay"] != "0";
    HWND hwnd = look::GameWindow(g_pid);
    if (!hwnd) return "error: no visible game window";
    std::vector<uint8_t> px; int w = 0, h = 0;
    // Window capture first (works behind other windows); desktop duplication only if it is unavailable.
    std::string e = wgc::Grab(hwnd, px, w, h), via = "window";
    if (!e.empty() && e.find("minimized") == std::string::npos) {
        std::string e2 = grab.Grab(hwnd, px, w, h);
        if (e2.empty()) { via = "screen"; e.clear(); } else e += "; " + e2;
    }
    if (!e.empty()) return "error: " + e;
    (void)via;
    State s = ReadState();
    std::vector<look::Label> labels; std::string header;
    AglCamera cam;
    if (overlay && agl_read(&g_link->camera, sizeof cam, &cam) && cam.valid) {
        float u, v;
        if (s.has_pos && Project(cam, s.pos, u, v)) labels.push_back({u, v, s.proxy ? "pivot" : "you", RGB(120, 255, 120)});
        for (auto& [n, p] : g_marks)
            if (Project(cam, p, u, v)) labels.push_back({u, v, Fmt("%s %.1fm", n.c_str(), Len(p - s.pos)), RGB(255, 220, 80)});
        for (auto& en : s.ents)
            if (Project(cam, en.pos, u, v))
                labels.push_back({u, v, Fmt("%s#%u", en.name.empty() ? "entity" : en.name.c_str(), en.id), en.kind == 1 ? RGB(255, 90, 90) : RGB(120, 200, 255)});
    }
    if (overlay && s.has_pos) header = Fmt("pos %.1f %.1f %.1f  facing %.0f", s.pos.x, s.pos.y, s.pos.z, Yaw(Norm(Ground(s.cam_fwd, g_prof.up))));
    std::string save = o.count("save") ? o["save"] : "";
    for (char& ch : save) if (!isalnum((unsigned char)ch) && ch != '-' && ch != '_') ch = '_';
    const std::string path = g_dir + "\\logs\\" + (save.empty() ? std::string("look") : g_game + "-" + save) + ".jpg";
    int ow, oh;
    e = look::Save(px, w, h, width, labels, header, std::wstring(path.begin(), path.end()), ow, oh);
    if (!e.empty()) return "error: " + e;
    return Fmt("image %s %dx%d\n", path.c_str(), ow, oh) + Describe(g_prof, s);
}

static std::string Handle(const std::string& line) {
    std::istringstream in(line); std::string cmd; in >> cmd;
    if (cmd == "ping") { State s = ReadState(); return Fmt("pong pid=%u game=%s host=%u caps=%u pos=%s", g_pid, g_game.c_str(), g_link->h.host_kind, g_link->h.caps, s.has_pos ? "yes" : "no"); }
    if (cmd == "state") {
        std::string ev = EventsText(g_ev_pending); g_ev_pending = 0;
        return Describe(g_prof, ReadState()) + (ev.empty() ? "" : "\nevents since last call: " + ev);
    }
    if (cmd == "goto") return CmdGoto(in);
    if (cmd == "face") return CmdFace(in);
    if (cmd == "do") { std::string rest; std::getline(in, rest); return CmdDo(rest); }
    if (cmd == "wait") return CmdWait(in);
    if (cmd == "mark") {
        std::string n; in >> n; if (n.empty()) return "error: mark <name>";
        State s = ReadState(); if (!s.has_pos) return "error: no position";
        g_marks[n] = s.pos; SaveMarks(); return Fmt("marked %s at %.1f %.1f %.1f", n.c_str(), s.pos.x, s.pos.y, s.pos.z);
    }
    if (cmd == "unmark") { std::string n; in >> n; g_marks.erase(n); SaveMarks(); return "ok"; }
    if (cmd == "marks") {
        State s = ReadState(); std::string r;
        for (auto& [n, p] : g_marks) r += Fmt("%s %.1f %.1f %.1f (%.1f m)\n", n.c_str(), p.x, p.y, p.z, s.has_pos ? Len(p - s.pos) : -1.f);
        return r.empty() ? "no marks" : r;
    }
    if (cmd == "calibrate") return CmdCalibrate();
    if (cmd == "look") return CmdLook(in);
    if (cmd == "tp") {
        if (!(g_link->h.caps & AGL_CAP_TELEPORT)) return "error: this host cannot teleport";
        std::string t; in >> t; State s = ReadState(); V3 p;
        if (!Target(t, s, p)) return "error: tp <x,y,z|mark>";
        AglCommand& c = g_link->cmd;
        agl_begin(&c.seq);
        c.cmd_seq++; c.ttl_ms = 300; c.buttons = 0; c.flags = 1;
        for (int i = 0; i < 4; ++i) c.stick[i] = 0;
        c.move[0] = c.move[1] = c.move[2] = 0;
        c.teleport[0] = p.x; c.teleport[1] = p.y; c.teleport[2] = p.z;
        c.issued_ms = agl_now_ms();
        agl_end(&c.seq);
        Sleep(400);
        State e = ReadState();
        return Fmt("teleported, %.1f m from target\n", Len(e.pos - p)) + Describe(g_prof, e);
    }
    if (cmd == "stop") { Release(); return "ok"; }
    if (cmd == "graph") return Fmt("walk graph: %zu nodes, %zu edges (%s)", g_graph.nodes.size(), g_graph.Edges(), GraphPath().c_str());
    return "error: commands: ping state goto face do wait look tp mark unmark marks calibrate graph stop";
}
static std::string HandleAndSave(const std::string& line) {
    std::string r = Handle(line);
    if (g_graph.dirty) SaveGraph();
    return r;
}

int main(int argc, char** argv) {
    int port = 27070;
    for (int i = 1; i + 1 < argc; ++i) {
        std::string a = argv[i];
        if (a == "--pid") g_pid = (uint32_t)atoi(argv[++i]);
        else if (a == "--port") port = atoi(argv[++i]);
        else if (a == "--profile") g_prof_path = argv[++i];
        else if (a == "--dir") g_dir = argv[++i];
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); // window rects in real pixels
    if (!g_pid) g_pid = FindHostPid();
    if (!g_pid || !(g_link = agl_open(g_pid, 0))) { std::fprintf(stderr, "agentd: no AgentLink host found\n"); return 2; }
    g_proc = OpenProcess(SYNCHRONIZE, FALSE, g_pid);
    g_game.assign(g_link->h.game, strnlen(g_link->h.game, 32));
    if (g_dir.empty()) { char b[MAX_PATH]; GetModuleFileNameA(nullptr, b, MAX_PATH); g_dir = b; g_dir = g_dir.substr(0, g_dir.find_last_of('\\')) + "\\..\\agent"; }
    CreateDirectoryA(g_dir.c_str(), nullptr);
    CreateDirectoryA((g_dir + "\\profiles").c_str(), nullptr);
    CreateDirectoryA((g_dir + "\\logs").c_str(), nullptr);
    if (g_prof_path.empty()) g_prof_path = g_dir + "\\profiles\\" + g_game + ".ini";
    g_prof.up = (int)g_link->h.up_axis ? (int)g_link->h.up_axis : 1;
    g_prof.pad = (g_link->h.caps & AGL_CAP_STICKS) != 0;
    if (!g_prof.pad) g_prof.right_sign = g_prof.up == 2 ? -1.f : 1.f;
    LoadProfile(g_prof_path);
    LoadMarks();
    if (!g_graph.Load(GraphPath())) { ImportFlightLogs(); if (g_graph.dirty) SaveGraph(); }
    SYSTEMTIME st; GetLocalTime(&st);
    g_flight = std::fopen(Fmt("%s\\logs\\%s-%04d%02d%02d-%02d%02d%02d.jsonl", g_dir.c_str(), g_game.c_str(), st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond).c_str(), "w");
    timeBeginPeriod(1);

    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons((u_short)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    BOOL excl = TRUE; setsockopt(ls, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&excl, sizeof excl);
    if (bind(ls, (sockaddr*)&addr, sizeof addr) || listen(ls, 4)) { std::fprintf(stderr, "agentd: port %d busy\n", port); return 3; }
    std::printf("agentd: pid %u game %s port %d profile %s\n", g_pid, g_game.c_str(), port, g_prof_path.c_str());
    std::fflush(stdout);
    // Exit with the game.
    std::thread([] { WaitForSingleObject(g_proc, INFINITE); Release(); ExitProcess(0); }).detach();
    // One thread per client (the MCP server keeps its connection open; scripts connect beside it). Commands still
    // run one at a time under g_cmd: a long goto/do makes the other client wait.
    for (;;) {
        SOCKET c = accept(ls, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;
        std::thread([c] {
            std::string buf; char tmp[4096]; int n;
            while ((n = recv(c, tmp, sizeof tmp, 0)) > 0) {
                buf.append(tmp, n);
                size_t nl;
                while ((nl = buf.find('\n')) != std::string::npos) {
                    std::string line = buf.substr(0, nl); buf.erase(0, nl + 1);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    std::string r;
                    if (!line.empty()) { std::lock_guard<std::mutex> lk(g_cmd); r = HandleAndSave(line); }
                    r += "\n.\n";
                    send(c, r.c_str(), (int)r.size(), 0);
                }
            }
            closesocket(c);
        }).detach();
    }
}
