// simhost: the toy world (sim.h) behind a real AgentLink, so agentd can be tested end to end without a game.
// Applies cmd like a pad host: ignores commands older than ttl_ms. Prints its pid, then runs until killed.
//   simhost [--walls] [--longwall]   (short wall for the unstuck test, long wall for the walk-graph test)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include "sim.h"
#include <cstdio>
#include <cstring>
#pragma comment(lib, "winmm.lib")

using namespace agent;

int main(int argc, char** argv) {
    AglLink* l = agl_open(GetCurrentProcessId(), 1);
    if (!l) return 2;
    l->h.host_kind = AGL_HOST_SIM; l->h.up_axis = 1; l->h.caps = AGL_CAP_STICKS;
    std::strcpy(l->h.game, "sim");
    sim::World w;
    bool longwall = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--walls")) w.walls.push_back({-1.2f, 4, 1.2f, 4.5f});
        if (!std::strcmp(argv[i], "--longwall")) { w.walls.push_back({-8, 4, 8, 5}); longwall = true; } // walk-graph test
    }
    std::printf("%lu\n", GetCurrentProcessId());
    std::fflush(stdout);
    timeBeginPeriod(1);
    uint64_t frame = 0;
    double last = agl_now_ms();
    for (;;) {
        Sleep(16);
        const double now = agl_now_ms();
        AglCommand c; Output o;
        if (agl_read(&l->cmd, sizeof c, &c) && c.issued_ms > 0 && now - c.issued_ms <= c.ttl_ms) {
            for (int i = 0; i < 4; ++i) o.stick[i] = c.stick[i];
            o.buttons = c.buttons;
            l->h.reserved[1] = c.cmd_seq;
        }
        // Test hook: touchpad (bit 11) press = take 10 damage, to raise a `damaged` event on demand.
        static bool pad_was = false;
        const bool pad = (o.buttons >> 11) & 1;
        if (pad && !pad_was) w.hp -= 10;
        pad_was = pad;
        // Test hook: d-pad up (bit 12) press closes the "door" (the long wall), for the walk-graph forget test.
        static bool up_was = false;
        const bool upb = (o.buttons >> 12) & 1;
        if (upb && !up_was && !longwall) { w.walls.push_back({-8, 4, 8, 5}); longwall = true; }
        up_was = upb;
        w.Step(o, (float)(now - last));
        last = now; ++frame;
        State s = w.Observe();
        agl_begin(&l->player.seq);
        l->player.valid = AGL_V_POS | AGL_V_VEL | AGL_V_HP | AGL_V_FLAGS; l->player.frame_id = frame;
        l->player.pos[0] = s.pos.x; l->player.pos[1] = s.pos.y; l->player.pos[2] = s.pos.z;
        l->player.vel[0] = s.vel.x; l->player.vel[1] = s.vel.y; l->player.vel[2] = s.vel.z;
        l->player.hp = s.hp; l->player.hp_max = s.hp_max; l->player.flags = s.flags;
        agl_end(&l->player.seq);
        agl_begin(&l->camera.seq);
        l->camera.valid = 1; l->camera.frame_id = frame;
        l->camera.fwd[0] = s.cam_fwd.x; l->camera.fwd[1] = s.cam_fwd.y; l->camera.fwd[2] = s.cam_fwd.z;
        agl_end(&l->camera.seq);
        if (longwall) { // one enemy behind the long wall, a goto target by id (#7)
            agl_begin(&l->ents.seq);
            l->ents.count = 1; l->ents.e[0] = AglEntity{};
            l->ents.e[0].id = 7; l->ents.e[0].kind = 1; l->ents.e[0].pos[2] = 10; l->ents.e[0].hp_pct = 1;
            std::strcpy(l->ents.e[0].name, "Crawler");
            agl_end(&l->ents.seq);
        }
    }
}
