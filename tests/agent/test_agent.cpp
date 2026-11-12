// Unit tests for agentlink.h + agentcore.h against the toy world (tests/agent/sim.h).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "sim.h"
#include <atomic>
#include <thread>
#include <cstdio>

using namespace agent;
using namespace sim;
static int g_fail, g_pass;
#define CHECK(c) do { if (c) g_pass++; else { g_fail++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static Profile BB() { Profile p; return p; }

static void TestSeqlock() {
    static AglLink link{};
    std::atomic<bool> stop{false};
    std::thread wr([&] {
        for (uint32_t i = 1; !stop; ++i) {
            agl_begin(&link.camera.seq);
            for (float& v : link.camera.view) v = (float)i;
            link.camera.frame_id = i;
            agl_end(&link.camera.seq);
        }
    });
    int torn = 0, ok = 0;
    for (int n = 0; n < 200000; ++n) {
        AglCamera c;
        if (!agl_read(&link.camera, sizeof c, &c)) continue;
        ++ok;
        for (float v : c.view) if (v != (float)c.frame_id) { ++torn; break; }
    }
    stop = true; wr.join();
    printf("seqlock: %d reads, %d torn\n", ok, torn);
    CHECK(ok > 1000);
    CHECK(torn == 0);
}

static void TestOpenField() {
    for (float ctrl : {16.f, 90.f}) { // 60 Hz and the ~11 Hz RT case
        World w; Profile p = BB();
        auto r = RunGoto(w, p, {{0, 0, 15}}, 0.8f, ctrl);
        double ideal = 15 / 3.3 * 1000 + 200;
        printf("open 15 m @%.0f ms: %s in %.0f ms (ideal %.0f), end %.2f m, flips %d\n", ctrl, StatusName(r.status), r.ms, ideal, r.final_dist, r.flips);
        CHECK(r.status == Status::Arrived);
        CHECK(r.ms < ideal * 1.3);
        CHECK(r.final_dist < 1.2f);
        CHECK(r.flips == 0);
    }
}

static void TestDiagonalAndCameraOff() {
    World w; w.cam_yaw = 120; // camera looks away from the target
    auto r = RunGoto(w, BB(), {{-8, 0, 9}}, 0.8f);
    printf("diagonal, camera away: %s %.0f ms end %.2f\n", StatusName(r.status), r.ms, r.final_dist);
    CHECK(r.status == Status::Arrived);
    CHECK(r.final_dist < 1.2f);
}

static void TestShortHop() {
    World w;
    auto r = RunGoto(w, BB(), {{1.5f, 0, 1.5f}}, 0.6f);
    printf("short hop 2.1 m: %s %.0f ms end %.2f\n", StatusName(r.status), r.ms, r.final_dist);
    CHECK(r.status == Status::Arrived);
    CHECK(r.final_dist < 0.9f);
}

static void TestWaypoints() {
    World w; w.walls.push_back({-3, 4, 3, 5}); // wall across the straight line
    auto r = RunGoto(w, BB(), {{4.5f, 0, 2}, {4.5f, 0, 7}, {0, 0, 10}}, 0.8f);
    printf("waypoints around wall: %s %.0f ms end %.2f\n", StatusName(r.status), r.ms, r.final_dist);
    CHECK(r.status == Status::Arrived);
}

static void TestStuckSmallWall() {
    World w; w.walls.push_back({-1.2f, 4, 1.2f, 4.5f}); // short wall: the sidestep gets around it
    auto r = RunGoto(w, BB(), {{0, 0, 9}}, 0.8f);
    printf("short wall in the way: %s %.0f ms end %.2f\n", StatusName(r.status), r.ms, r.final_dist);
    CHECK(r.status == Status::Arrived);
}

static void TestStuckPocket() {
    World w; // U pocket opening toward the pawn, target behind its bottom
    w.walls = {{-6, 5, 6, 5.5f}, {-6, 0, -5.5f, 5.5f}, {5.5f, 0, 6, 5.5f}};
    w.pos = {0, 1};
    auto r = RunGoto(w, BB(), {{0, 0, 9}}, 0.8f);
    printf("U pocket: %s %.0f ms\n", StatusName(r.status), r.ms);
    CHECK(r.status == Status::Stuck || r.status == Status::Arrived);
    CHECK(r.ms < 10000);
}

static void TestNoFalseStuck() {
    World w; w.noise = 0.05f; // pivot jitter
    auto r = RunGoto(w, BB(), {{0, 0, 25}}, 0.8f);
    CHECK(r.status == Status::Arrived);
    World w2; // slow walk: walls nowhere, but a low run speed
    w2.run = 1.4f; Profile p = BB(); p.run_speed = 1.4f; p.brake = 0.2f;
    r = RunGoto(w2, p, {{0, 0, 8}}, 0.8f);
    printf("noise/slow: %s\n", StatusName(r.status));
    CHECK(r.status == Status::Arrived);
}

static void TestWorldMoveHost() {
    World w; w.up = 2; w.pad = false; Profile p; p.up = 2; p.pad = false; p.right_sign = -1;
    auto r = RunGoto(w, p, {{10, -6, 0}}, 0.8f);
    printf("world-move host (Z up): %s %.0f ms end %.2f\n", StatusName(r.status), r.ms, r.final_dist);
    CHECK(r.status == Status::Arrived);
}

// Target straight above/below on another level: same ground spot is not "arrived" (live BB 2026-11-11: 19.6 m off).
static void TestOtherLevel() {
    World w;
    auto r = RunGoto(w, BB(), {{0, 19.6f, 6}}, 0.8f);
    printf("target on another level: %s\n", StatusName(r.status));
    CHECK(r.status == Status::WrongLevel);
    World w2; // a small step up still counts
    r = RunGoto(w2, BB(), {{0, 1.2f, 6}}, 0.8f);
    CHECK(r.status == Status::Arrived);
}

static void TestEvents() {
    World w; EventWatch ev; ev.enemy_r = 5;
    CHECK(ev.Step(w.Observe()) == 0);
    w.hp = 80; CHECK(ev.Step(w.Observe()) == EV_DAMAGED);
    CHECK(ev.Step(w.Observe()) == 0); // edge triggered
    w.hp = 20; CHECK(ev.Step(w.Observe()) == (EV_DAMAGED | EV_HP_LOW));
    w.hp = 15; CHECK(ev.Step(w.Observe()) == EV_DAMAGED);
    Entity e; e.kind = 1; e.pos = {0, 0, 3}; w.ents.push_back(e);
    CHECK(ev.Step(w.Observe()) == EV_ENEMY_NEAR);
    CHECK(ev.Step(w.Observe()) == 0);
    // goto stops on an event it was asked about
    Goto g; g.pts = {{0, 0, 20}}; g.stop_on = EV_DAMAGED; Profile p;
    State s = w.Observe(); g.Begin(p, s); g.Step(p, s, 0); g.Step(p, s, EV_ENEMY_NEAR);
    CHECK(g.status == Status::Running);
    g.Step(p, s, EV_DAMAGED);
    CHECK(g.status == Status::Event && g.event == EV_DAMAGED);
}

static void TestFace() {
    for (float target : {90.f, -150.f, 10.f}) {
        World w; Face f; f.yaw = target; Profile p;
        State s = w.Observe(); f.Begin(s);
        while (f.status == Status::Running && w.t_ms < 8000) { s = w.Observe(); Output o = f.Step(p, s); w.Step(o, 16); }
        printf("face %.0f: %s at %.0f ms, yaw %.1f\n", target, StatusName(f.status), w.t_ms, w.cam_yaw);
        CHECK(f.status == Status::Aligned);
        CHECK(std::fabs(Wrap180(w.cam_yaw - target)) < 8);
    }
}

static void TestDescribe() {
    World w; Profile p;
    Entity a; a.id = 3; a.kind = 1; a.name = "Huntsman"; a.pos = {0, 0, 7};
    Entity b; b.id = 4; b.kind = 2; b.name = "Doll"; b.pos = {-5, 0, 0};
    w.ents = {b, a};
    std::string t = Describe(p, w.Observe());
    printf("describe:\n%s\n", t.c_str());
    CHECK(t.find("Doll#4 5.0 m left 90 deg") != std::string::npos);
    CHECK(t.find("Huntsman#3 7.0 m ahead") != std::string::npos);
    CHECK(t.size() < 600);
}

// Walk graph: learn a route around a long wall once, then a plain goto to the far side plans through it.
static void TestWalkGraphAroundWall() {
    const std::vector<Rect> walls = {{-8, 4, 8, 5}};
    World w0; w0.walls = walls;
    auto straight = RunGoto(w0, BB(), {{0, 0, 10}}, 0.8f, 16, 20000);
    printf("graph: straight goto at a long wall: %s\n", StatusName(straight.status));
    CHECK(straight.status == Status::Stuck);

    World w; w.walls = walls; WalkGraph gr;
    auto learn = RunGoto(w, BB(), {{9.5f, 0, 2}, {9.5f, 0, 7}, {0, 0, 10}}, 0.8f, 16, 60000, &gr);
    CHECK(learn.status == Status::Arrived);
    printf("graph: learned %zu nodes, %zu edges\n", gr.nodes.size(), gr.Edges());
    CHECK(gr.nodes.size() > 5);

    auto plan = gr.Plan({0, 0, 0}, {0, 0, 10}, 1);
    printf("graph: plan %zu waypoints\n", plan.size());
    CHECK(plan.size() >= 3);
    World w2; w2.walls = walls;
    auto r = RunGoto(w2, BB(), plan, 0.8f, 16, 60000);
    printf("graph: planned goto %s %.0f ms end %.2f\n", StatusName(r.status), r.ms, r.final_dist);
    CHECK(r.status == Status::Arrived);

    auto back = gr.Plan({0, 0, 10}, {0, 0, 0}, 1); // flat ground walks both ways
    CHECK(back.size() >= 3);
    World w3; w3.walls = walls; w3.pos = {0, 10};
    CHECK(RunGoto(w3, BB(), back, 0.8f, 16, 60000).status == Status::Arrived);

    CHECK(gr.Plan({0, 0, 0}, {30, 0, 30}, 1).empty()); // target far from known ground: no plan, goto walks straight
    CHECK(gr.Plan({0, 0, 0}, {-3, -1.8f, 10}, 1).size() >= 3); // an enemy 3 m off the track, 1.8 m below the pivot
    CHECK(gr.Plan({0, 0, 0}, {0, -4, 10}, 1).empty());          // but not one on another floor
}

static void TestWalkGraphLevels() {
    // Stairs (0.5 m rise per metre) walk both ways; a 10 m drop is one way.
    WalkGraph s; double t = 0;
    for (float z = 0; z <= 10; z += 0.3f, t += 100) s.Add({0, z * 0.5f, z}, t, 1);
    CHECK(!s.Plan({0, 0, 0}, {0, 5, 10}, 1).empty());
    CHECK(!s.Plan({0, 5, 10}, {0, 0, 0}, 1).empty());

    WalkGraph d; t = 0;
    for (float z = 0; z <= 5; z += 0.3f, t += 100) d.Add({0, 10, z}, t, 1);   // ledge
    for (float y = 10; y > 0; y -= 1.5f, t += 100) d.Add({0, y, 5.3f}, t, 1); // fall
    for (float z = 5.3f; z <= 10; z += 0.3f, t += 100) d.Add({0, 0, z}, t, 1); // floor below
    CHECK(!d.Plan({0, 10, 0}, {0, 0, 10}, 1).empty());
    CHECK(d.Plan({0, 0, 10}, {0, 10, 0}, 1).empty());

    WalkGraph j; // a teleport after a pause makes no edge
    j.Add({0, 0, 0}, 0, 1); j.Add({0, 0, 1}, 100, 1); j.Add({0, 0, 2}, 200, 1);
    j.Add({50, 0, 50}, 3000, 1); j.Add({50, 0, 51}, 3100, 1);
    CHECK(j.Plan({0, 0, 0}, {50, 0, 51}, 1).empty());

    // The same street logged twice at different pivot heights (host change): the nearest node is on the dead-end
    // copy, the plan must still find the connected one.
    WalkGraph two; t = 0;
    for (float z = 0; z <= 10; z += 0.3f, t += 100) two.Add({0, 1.4f, z}, t, 1);
    t += 5000;
    for (float z = 0; z <= 20; z += 0.3f, t += 100) two.Add({0.3f, 0, z}, t, 1);
    CHECK(!two.Plan({0.05f, 0.7f, 5}, {0.3f, 0, 19}, 1).empty());

    char tmp[MAX_PATH]; GetTempPathA(MAX_PATH, tmp);
    const std::string f = std::string(tmp) + "walkgraph_test.graph";
    CHECK(s.Save(f));
    WalkGraph l; CHECK(l.Load(f));
    CHECK(l.nodes.size() == s.nodes.size() && l.Edges() == s.Edges());
    CHECK(l.Plan({0, 5, 10}, {0, 0, 0}, 1).size() == s.Plan({0, 5, 10}, {0, 0, 0}, 1).size());
    DeleteFileA(f.c_str());
    printf("graph levels: stairs %zu nodes, drop %zu nodes\n", s.nodes.size(), d.nodes.size());
}

// A door closes on a learned route: the plan walks into it, gets stuck, Forget drops that leg, the next plan takes
// the other learned route around.
static void TestWalkGraphForget() {
    WalkGraph gr;
    World open1; RunGoto(open1, BB(), {{0, 0, 10}}, 0.8f, 16, 30000, &gr); gr.Break();
    World open2; RunGoto(open2, BB(), {{9.5f, 0, 2}, {9.5f, 0, 7}, {0, 0, 10}}, 0.8f, 16, 60000, &gr); gr.Break();
    const std::vector<Rect> door = {{-8, 4, 8, 5}};
    int stucks = 0, forgot = 0; bool arrived = false;
    for (int i = 0; i < 4 && !arrived; ++i) {
        auto plan = gr.Plan({0, 0, 0}, {0, 0, 10}, 1);
        if (plan.empty()) break;
        World w; w.walls = door;
        Goto g; g.pts = plan; g.tol = 0.8f; g.timeout_ms = 30000;
        State s = w.Observe(); g.Begin(BB(), s);
        while (g.status == Status::Running && w.t_ms < 31000) { s = w.Observe(); Output o = g.Step(BB(), s, 0); w.Step(o, 16); }
        if (g.status == Status::Arrived) { arrived = true; break; }
        if (g.status != Status::Stuck) break;
        ++stucks;
        const size_t k = g.Leg();
        forgot += gr.Forget(k ? plan[k - 1] : V3{0, 0, 0}, plan[k], s.pos, 1);
    }
    printf("graph forget: %d stuck goto(s), %d edge(s) forgotten, then %s\n", stucks, forgot, arrived ? "arrived around" : "no route");
    CHECK(stucks == 1 && forgot > 0 && arrived);
}

int main() {
    TestSeqlock();
    TestOpenField();
    TestDiagonalAndCameraOff();
    TestShortHop();
    TestWaypoints();
    TestStuckSmallWall();
    TestStuckPocket();
    TestNoFalseStuck();
    TestWorldMoveHost();
    TestOtherLevel();
    TestEvents();
    TestFace();
    TestDescribe();
    TestWalkGraphAroundWall();
    TestWalkGraphLevels();
    TestWalkGraphForget();
    printf("agent tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
