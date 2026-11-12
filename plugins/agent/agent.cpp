// AgentLink host for Unreal games (docs/agent): publishes player + camera each game tick and applies agentd's
// command (world move via AddMovementInput, look via SetControlRotation, buttons as keys via `bot`, teleport).
// Units: metres (UE cm / 100), Z up, host_kind UE. Yaw on the link follows agentcore: dir = (sin y, cos y) in (x, y).
//   agent status | agent keys cross=space circle=c ... | agent off | agent on
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "crawler_api.h"
#include "../../src/agent/agentlink.h"
#pragma comment(lib, "user32.lib")

static const UEApi* api;
static AglLink* g_link;
static int g_off;
static uint64_t g_frames, g_applied, g_last_cmd_seq, g_tp_seq;
static uint32_t g_down;              // buttons currently held as keys
static char g_keys[16][24] = {"space", "c", "f", "r", "q", "e", "rmb", "lmb", "shift", "v", "esc", "tab", "1", "2", "3", "4"};
static const char* kBtn[16] = {"cross", "circle", "square", "triangle", "l1", "r1", "l2", "r2", "l3", "r3",
                               "options", "touchpad", "up", "down", "left", "right"};
static double g_prev_pos[3], g_prev_ms;
static float g_vp_w = 1920, g_vp_h = 1080;
static int g_move_mode, g_latched; // 0 unknown, 1 SetMoveVector (latched), 2 AddMovementInput (per frame)
static double g_tick_ms_sum; static uint64_t g_tick_n;

// Pulls n floats named by keys ("X=", "Y=", ...) out of a crawler value text like "Vector{X=1 Y=2 Z=3}".
static bool Floats(const char* s, const char* const* keys, int n, double* out) {
    for (int i = 0; i < n; ++i) {
        const char* p = strstr(s, keys[i]);
        if (!p) return false;
        out[i] = atof(p + strlen(keys[i]));
    }
    return true;
}
static const char* kXYZ[3] = {"X=", "Y=", "Z="};
static const char* kPYR[3] = {"Pitch=", "Yaw=", "Roll="};

static bool GetVec(uintptr_t o, const char* path, double v[3]) {
    char b[512];
    return o && api->get(o, path, b, sizeof b) == 1 && Floats(b, kXYZ, 3, v);
}
static bool GetRot(uintptr_t o, const char* path, double r[3]) {
    char b[512];
    return o && api->get(o, path, b, sizeof b) == 1 && Floats(b, kPYR, 3, r);
}
static bool GetF(uintptr_t o, const char* path, double& f) {
    char b[256];
    if (!o || api->get(o, path, b, sizeof b) != 1) return false;
    const char* e = strrchr(b, '=');
    f = atof(e ? e + 1 : b);
    return true;
}

// Object property value -> pointer (0 when None). Takes "0x<value> Class Name" (api->get) or the CLI form
// "ObjectProperty Pawn @0x<field> = 0x<value> Class Name".
static uintptr_t PtrProp(uintptr_t o, const char* path) {
    char b[256];
    if (!o || api->get(o, path, b, sizeof b) != 1) return 0; // -1 = error text in b
    const char* eq = strstr(b, "= ");
    const char* h = strstr(eq ? eq : b, "0x");
    return h ? (uintptr_t)_strtoui64(h, nullptr, 16) : 0;
}

static void Key(int i, bool down) {
    char line[64], out[256];
    snprintf(line, sizeof line, "bot key %s %s", g_keys[i], down ? "down" : "up");
    api->cmd(line, out, sizeof out);
}

static void Publish(uintptr_t pc, uintptr_t pawn) {
    const double now = agl_now_ms();
    agl_begin(&g_link->frame.seq);
    g_link->frame.frame_id = ++g_frames;
    g_link->frame.dt_ms = (float)(now - g_link->frame.host_ms);
    g_link->frame.host_ms = now;
    agl_end(&g_link->frame.seq);

    double loc[3], rot[3];
    char lb[512];
    if (pawn && api->call(pawn, "K2_GetActorLocation", "", lb, sizeof lb) && Floats(lb, kXYZ, 3, loc)) {
        float vel[3] = {0, 0, 0};
        const double dt = (now - g_prev_ms) / 1000;
        if (g_prev_ms > 0 && dt > 0 && dt < 0.5)
            for (int i = 0; i < 3; ++i) vel[i] = (float)((loc[i] - g_prev_pos[i]) / 100 / dt);
        memcpy(g_prev_pos, loc, sizeof loc); g_prev_ms = now;
        const bool has_rot = GetRot(pc, "ControlRotation", rot); // crawler calls stay outside the seqlock
        AglPlayer& p = g_link->player;
        agl_begin(&p.seq);
        p.frame_id = g_frames;
        p.valid = AGL_V_POS | AGL_V_VEL | AGL_V_FLAGS;
        for (int i = 0; i < 3; ++i) { p.pos[i] = (float)(loc[i] / 100); p.vel[i] = vel[i]; }
        if (has_rot) { p.yaw = (float)(90 - rot[1]); p.pitch = (float)rot[0]; p.valid |= AGL_V_YAW; }
        p.flags = AGL_F_ALIVE;
        agl_end(&p.seq);
    }
    // Camera: PlayerCameraManager's cached POV (UE4/5), fallback to the control rotation at the pawn.
    const uintptr_t pcm = PtrProp(pc, "PlayerCameraManager");
    double cl[3], cr[3], fov = 90;
    bool ok = pcm && GetVec(pcm, "CameraCachePrivate.POV.Location", cl) && GetRot(pcm, "CameraCachePrivate.POV.Rotation", cr);
    if (ok) GetF(pcm, "CameraCachePrivate.POV.FOV", fov);
    else if (GetRot(pc, "ControlRotation", cr)) { memcpy(cl, g_prev_pos, sizeof cl); cl[2] += 160; ok = true; }
    if (!ok) return;
    const double d2r = 3.14159265358979 / 180, cp = cos(cr[0] * d2r), sp = sin(cr[0] * d2r), cy = cos(cr[1] * d2r), sy = sin(cr[1] * d2r);
    const double L[3] = {cl[0] / 100, cl[1] / 100, cl[2] / 100};
    const double f[3] = {cp * cy, cp * sy, sp}, r[3] = {-sy, cy, 0}, u[3] = {-sp * cy, -sp * sy, cp};
    AglCamera& c = g_link->camera;
    agl_begin(&c.seq);
    c.frame_id = g_frames; c.valid = 1;
    for (int i = 0; i < 3; ++i) { c.pos[i] = (float)L[i]; c.fwd[i] = (float)f[i]; }
    const double* rows[3] = {r, u, f};
    for (int k = 0; k < 3; ++k) {
        for (int i = 0; i < 3; ++i) { c.view[k * 4 + i] = (float)rows[k][i]; c.inv_view[i * 4 + k] = (float)rows[k][i]; }
        c.view[k * 4 + 3] = (float)-(rows[k][0] * L[0] + rows[k][1] * L[1] + rows[k][2] * L[2]);
        c.inv_view[k * 4 + 3] = (float)L[k];
    }
    const float t = (float)(1 / tan(fov * d2r / 2));
    c.proj[0] = t; c.proj[1] = t * g_vp_w / g_vp_h; c.proj[2] = 0; c.proj[3] = 0;
    c.render_w = (uint32_t)g_vp_w; c.render_h = (uint32_t)g_vp_h;
    agl_end(&c.seq);
}

static void Apply(uintptr_t pc, uintptr_t pawn) {
    AglCommand c;
    char args[160], out[512];
    const bool fresh = agl_read(&g_link->cmd, sizeof c, &c) && c.issued_ms > 0 && agl_now_ms() - c.issued_ms <= c.ttl_ms;
    const uint32_t want = fresh ? c.buttons : 0;
    for (int i = 0; i < 16; ++i)
        if (((want ^ g_down) >> i) & 1) Key(i, (want >> i) & 1);
    g_down = want;
    const double m = fresh ? sqrt(c.move[0] * c.move[0] + c.move[1] * c.move[1] + c.move[2] * c.move[2]) : 0;
    // Latched mode (SetMoveVector): send zero once when the move stops or the command goes stale.
    if (g_move_mode == 1 && g_latched && m <= 0.01 && pawn) { api->call(pawn, "SetMoveVector", "0,0,0", out, sizeof out); g_latched = 0; }
    if (!fresh) return;
    g_link->h.reserved[1] = c.cmd_seq;
    if (m > 0.01 && pawn) {
        const double s = m > 1 ? 1 / m : 1; // clamp to unit
        if (g_move_mode != 2) { // SetMoveVector(forward, right) relative to the control yaw (STALKER 2 style pawns)
            double rot[3] = {0, 0, 0};
            GetRot(pc, "ControlRotation", rot);
            const double y = rot[1] * 3.14159265358979 / 180, fx = cos(y), fy = sin(y);
            const double mx = c.move[0] * s, my = c.move[1] * s;
            snprintf(args, sizeof args, "%.3f,%.3f,0", mx * fx + my * fy, -mx * fy + my * fx);
            const int ok = api->call(pawn, "SetMoveVector", args, out, sizeof out);
            if (g_move_mode == 0) { g_move_mode = ok ? 1 : 2; api->log("agent: move mode %s", ok ? "SetMoveVector" : "AddMovementInput"); }
            if (ok) g_latched = 1;
        }
        if (g_move_mode == 2) {
            char line[256];
            snprintf(line, sizeof line, "%f,%f,%f %f", c.move[0] / m, c.move[1] / m, c.move[2] / m, m * s);
            api->call(pawn, "AddMovementInput", line, out, sizeof out); // args space separated like the CLI
        }
        ++g_applied;
    }
    if (c.flags & 2) {
        double rot[3];
        if (GetRot(pc, "ControlRotation", rot)) {
            snprintf(args, sizeof args, "%f,%f,%f", rot[0], 90 - c.look[0], rot[2]);
            api->call(pc, "SetControlRotation", args, out, sizeof out);
        }
    }
    if ((c.flags & 1) && c.cmd_seq != g_tp_seq && pawn) {
        g_tp_seq = c.cmd_seq; // K2_TeleportTo refuses spots it finds blocked (STALKER 2): set the location, no sweep
        snprintf(args, sizeof args, "%f,%f,%f 0 0 1", c.teleport[0] * 100, c.teleport[1] * 100, c.teleport[2] * 100);
        api->call(pawn, "K2_SetActorLocation", args, out, sizeof out);
    }
}

UE_EXPORT int UEPluginInit(const UEApi* a) {
    api = a;
    if (getenv("AGENTLINK") && !strcmp(getenv("AGENTLINK"), "0")) { g_off = 1; return 1; }
    g_link = agl_open(GetCurrentProcessId(), 1);
    if (!g_link) { api->log("agent: AgentLink mapping failed"); return 1; }
    g_link->h.host_kind = AGL_HOST_UE; g_link->h.up_axis = 2;
    g_link->h.caps = AGL_CAP_WORLD_MOVE | AGL_CAP_LOOK | AGL_CAP_TELEPORT | AGL_CAP_KEYS;
    char exe[MAX_PATH], game[32]; GetModuleFileNameA(nullptr, exe, MAX_PATH);
    const char* base = strrchr(exe, '\\'); base = base ? base + 1 : exe;
    const char* env = getenv("AGENT_GAME");
    snprintf(game, sizeof game, "%s", env ? env : base);
    for (char* p = game; *p; ++p) { if (*p == '-' || *p == '.' || *p == '_') { *p = 0; break; } *p = (char)tolower(*p); }
    memcpy(g_link->h.game, game, sizeof g_link->h.game);
    api->log("agent: AgentLink Local\\AgentLink_%lu ready, game %s", GetCurrentProcessId(), game);
    return 1;
}

// api->find scans the whole object array (~100 ms on STALKER 2): never per tick. The PC is cached and checked
// through a cheap property read; the pawn comes from pc.Pawn.
static uintptr_t g_pc;

UE_EXPORT void UEPluginTick() {
    if (!g_link || g_off) return;
    const double t0 = agl_now_ms();
    static uint64_t check_t, scan_t;
    const uint64_t now_t = api->ticks();
    if (g_pc && now_t - check_t > 120) { // still a live controller? (level change frees it)
        check_t = now_t;
        if (!PtrProp(g_pc, "PlayerCameraManager")) g_pc = 0;
    }
    if (!g_pc && (!scan_t || now_t - scan_t > 300)) { scan_t = now_t; g_pc = api->find("pc"); }
    const uintptr_t pc = g_pc, pawn = PtrProp(pc, "Pawn");
    if (!pc) return;
    static uint64_t vp_t;
    if (api->ticks() - vp_t > 300) { // client rect of our largest window = what agent_look captures
        vp_t = api->ticks();
        struct Best { HWND h; long area; } best = {nullptr, 0};
        EnumWindows([](HWND h, LPARAM lp) -> BOOL {
            DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
            RECT r;
            if (pid == GetCurrentProcessId() && IsWindowVisible(h) && !GetWindow(h, GW_OWNER) && GetClientRect(h, &r)) {
                Best* b = (Best*)lp; const long a = (long)(r.right * r.bottom);
                if (a > b->area) { b->area = a; b->h = h; }
            }
            return TRUE;
        }, (LPARAM)&best);
        RECT r;
        if (best.h && GetClientRect(best.h, &r) && r.right > 0 && r.bottom > 0) { g_vp_w = (float)r.right; g_vp_h = (float)r.bottom; }
    }
    Publish(pc, pawn);
    Apply(pc, pawn);
    g_tick_ms_sum += agl_now_ms() - t0; ++g_tick_n;
}

UE_EXPORT void UEPluginShutdown() {
    for (int i = 0; i < 16; ++i) if ((g_down >> i) & 1) Key(i, false);
    g_down = 0;
}

UE_EXPORT const char* UEPluginHelp() { return "agent status | agent keys <button>=<key> ... | agent on | agent off"; }

UE_EXPORT int UEPluginCommand(const char* line, char* out, int n) {
    if (strncmp(line, "agent", 5) || (line[5] && line[5] != ' ')) return 0;
    const char* rest = line[5] ? line + 6 : "status";
    if (!strncmp(rest, "off", 3)) { g_off = 1; UEPluginShutdown(); snprintf(out, n, "agent host off"); return 1; }
    if (!strncmp(rest, "on", 2)) { g_off = 0; snprintf(out, n, "agent host on"); return 1; }
    if (!strncmp(rest, "keys", 4)) {
        char buf[512]; snprintf(buf, sizeof buf, "%s", rest + 4);
        for (char* tok = strtok(buf, " "); tok; tok = strtok(nullptr, " ")) {
            char* eq = strchr(tok, '='); if (!eq) continue; *eq = 0;
            for (int i = 0; i < 16; ++i) if (!strcmp(tok, kBtn[i])) snprintf(g_keys[i], sizeof g_keys[i], "%s", eq + 1);
        }
    }
    int w = snprintf(out, n, "agent host %s: link %s, frames %llu, tick %.2f ms avg, move mode %s, moves applied %llu, last cmd %llu, viewport %.0fx%.0f\nkeys:",
                     g_off ? "off" : "on", g_link ? "ok" : "none", (unsigned long long)g_frames, g_tick_n ? g_tick_ms_sum / g_tick_n : 0.0,
                     g_move_mode == 1 ? "SetMoveVector" : g_move_mode == 2 ? "AddMovementInput" : "?", (unsigned long long)g_applied,
                     (unsigned long long)(g_link ? g_link->h.reserved[1] : 0), g_vp_w, g_vp_h);
    g_tick_ms_sum = 0; g_tick_n = 0;
    for (int i = 0; i < 16 && w < n; ++i) w += snprintf(out + w, n - w, " %s=%s", kBtn[i], g_keys[i]);
    return 1;
}
