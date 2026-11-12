# Agent Layer: implementation plan (rev 2, after review)

Spec: `2026-11-11-agent-layer-design.md`. Bloodborne host: `docs/AGENT_HOST.md` in the private bbport repo.
Workflow: opus medium implements with unit TDD; supervisor proves live red/green (1-2 live checks per plan, invincible);
one final opus review per phase. Working first, refine last. No push/PR without the user; commits as the user's noreply.
Game Crawler is public: commit only generic docs/code, never bbport internals.

Status 2026-11-11: S2 measurement items (baseline runs, vision numbers, gameplay agent run) are **parked**; the S2
host is built and was checked live. Open items below are Bloodborne-only unless marked S2.

## Phase 0: baseline

- [x] `docs/agent/tasks.md`: the metric task per game (S2: bookmark ~50 m + nearest enemy; BB: from the Central Yharnam lamp).
- [x] (BB; S2 parked) 3 runs per game with today's tools (vision_grab + pad_hold / bot). Wall time, MCP calls, tokens, success.
      -> `docs/agent/baseline.md` + `~/.claude/improvements.md`. BB: 3 runs (2026-11-11/12), 0/3 finished:
      lost in the dark; ended 3.3 m and 4.0 m off the start with no signal.

## Phase 1: live spike (both hosts, throwaway-quality, proves the risky assumptions)

- [x] S1 S2: tiny plugin publishes pawn pos/yaw + frame id/dt once per frame; applies a fixed move vector once per frame
      via AddMovementInput. Measure: real frame Hz vs pump Hz, does S2 honour AddMovementInput at 0.3/0.6/1.0 magnitude.
- [x] S2 BB: camera block from the renderer + pad command merged in `sample()` (see AGENT_HOST.md). Measure: scePadRead Hz,
      command-to-motion latency in frames, camera validity (G-buffer frames, BB_UPSCALER), stick camera-relative or not.
- [x] S3 BB: player position. Check community offsets first (Bloodborne PS4 cheat tables / trainers, WorldChrMan-style
      structs), then memory diff driven by the pad against the camera track. Record result in AGENT_HOST.md.
- [x] Decision gate: write numbers in `docs/agent/spike.md`; adjust ABI/rates in the spec before Phase 2.

## Phase 2: core + sim + agentd (no game)

Files: `src/agent/agentlink.h`, `src/agent/agentcore.h`, `src/agent/agentd/*.cpp`, `tests/agent/*`;
add build to `src/build.bat` and tests to `tests/run_tests.bat` (CI runs it).

- [x] T1 `agentlink.h`: blocks, per-block seqlock helpers, valid masks. Test: 1 writer + 1 reader thread per block torture, no torn reads.
- [x] T2 in-process sim (test helper): floor, walls, U-wall, step, wandering entities, dt-driven kinematics, optional proxy noise.
- [x] T3 `agentcore` steering: goto arrives within tol, no overshoot loop, time <= 1.3x ideal, also at 11 Hz input.
- [x] T4 stuck ladder: U-wall -> recovers or returns `stuck` in < 6 s; proxy noise does not false-trigger.
- [x] T5 events + `stop_on` + `wait`: each fires once (edge triggered).
- [x] T6 agentd shell: map link by pid, TCP port = crawler port + 10, `ping/state/goto/face/do/wait/mark/tp/calibrate`,
      lock file (second client read-only), exits when pid dies, flight log JSONL. Test: fake host process.
- [x] T7 ttl: kill agentd mid-goto -> fake host input zero within ttl.

## Phase 3: MCP + vision

- [x] T8 `mcp/server.js`: image content support (`{text,image}` -> text + image content, outside the text cap);
      test in `tests/test_mcp.js`.
- [x] T9 `agent_*` tools, agentd auto-start per `game`, `withTimeout` for long calls; fake agentd test.
- [x] T10 state text format (nearest 8, bearing words, <= 600 chars); golden tests.
- [x] T11 `tools/agent_look.py` (dxcam + cv2): viewport crop with letterbox, projection, compass, JPEG.
      Test: synthetic camera + known world point -> marker pixel +-3, with and without letterbox.

## Phase 3b: vision bake-off, CPU vs GPU

V1 can start in Phase 3. V2-V4 need host state for the labels, so they run after T12/T13.

- [x] V1 `tools/agent_vision.py`: `hud` + `prompts`, backends `cpu`/`gpu` (DirectML). `moving`/`free_space` cut (agentcore
      has it / no consumer), `entities` on hold (memory), capture stays in agentd. Tests in `tests/agent/test_vision.py`.
- [x] V2 dataset with automatic labels (BB: 320 frames HUD + 7 hand-labelled OCR frames; S2 parked): record about 200 frames per game (S2 + BB) together with AgentLink state. Labels
      come from memory: entity boxes are projected world positions, HP is the memory value, moving/stuck comes from the
      position delta. Prompts are labelled by hand for about 30 frames.
- [x] V3 bench (BB done, S2 open) `tools/agent_vision_bench.py`, per signal x backend:
  - latency p50/p95 ms
  - accuracy: stuck F1, HP abs error, entity precision/recall, OCR exact match
  - VRAM
  - **game cost** = FPS drop while the vision loop runs at 5 Hz, on the S2 dev pair and on BB RT
  - captures: frames per second, and whether the game still captures while unfocused or covered
- [x] V4 decision table in `docs/agent/vision-bakeoff.md` + `~/.claude/improvements.md`.
  - Thresholds: stuck F1 >= 0.9, HP error <= 5%, entity recall >= 0.8, game FPS drop <= 5%.
  - Rule: pass the accuracy threshold first, then take the lower game cost; on a tie, CPU.
  - Wire the winners into agentd events and `agent_state`; drop the losers' dependencies.

## Phase 4: hosts for real

- [x] T12 UE plugin `plugins/agent`: per-frame state (pawn, ControlRotation, camera manager, profile hp/stamina/level),
      entities by profile classes (budgeted), apply command once per frame, buttons via `bot`, teleport, ttl. Profile `s2.json`.
- [x] T13 BB host per AGENT_HOST.md (renderer camera block, `sample()` pad merge incl. overlay early-return, player block
      from S3, `cheat_no_death` for live runs). Profile `bloodborne.json` with stick calibration.
- [x] Live (supervisor, 2 checks per host): metric task once; kill agentd mid-goto -> player stops.

## Phase 4b: gameplay agent

- [x] G1 `agents/gameplay.md`: frontmatter (name, description, tools allow-list, model opus, effort medium), playbook,
      brief/report format from the spec. Keep <= 5k tokens.
- [x] G2 `agent\notes\<game>.md` template + `agents/install.ps1 -Project <dir>` (copies into `.claude/agents/`).
- [x] G3 eval briefs `docs/agent/gameplay-evals.md`: the metric task, one QA repro (reach a place + do a skill + look),
      one review (3 checkpoints with images). Pass = report matches the flight log (no invented facts).
- [x] Live (supervisor): dispatch the agent with the metric task per host once; compare its calls/time to driving by hand.

## Phase 5: measure, keep or cut

- [x] (BB; S2 parked) Re-run Phase 0 (3 runs per game) + tick Hz + latency. Before/after table in `docs/agent/baseline.md` and
      `~/.claude/improvements.md`. Keep only what beats baseline. BB: 8 agent E1 runs, 8/8 reached (baseline 0/3).
- [x] Gameplay agent A/B: opus medium vs sonnet, metric task, 3 runs each; keep the faster one that still passes G3.
      -> sonnet medium (-21% wall, 3/3), see `baseline.md`.
- [x] If stuck rate > 10%: v2 learned walk graph + A* (+ UE navmesh). If look is too slow: WGC capture.
      BB raw 57% (8/14), but 0 stucks within 3 m of the target: all were straight gotos at walled targets (missing
      routes, not controller failures). Built 2026-11-12: `src/agent/walkgraph.h` (learned from every walked position
      + old flight logs, shortest path in goto, `#entity` targets, forgets edges where a planned goto got stuck). Live: e1_start <-> crawler pack (65 m, stairs) one plain goto
      each way, 17-19 s, 0 unstuck. UE navmesh not needed so far. Look already uses WGC (16-20 ms).
- [x] Docs: `docs/agent/README.md` (tools, profile format, ABI). Clean logs, sim artifacts, spike code.

## Later (v2+)

`agent_route` record/play for visual review, `agent_explore`, `agent_fight`, `drg` profile, vision-action model for
vision-only games (NitroGen-class) only if it beats baseline.

## Risks

| Risk | Mitigation |
|---|---|
| UE pump runs many times per frame (500 Hz cap, loader.cpp:62) | apply/publish once per frame keyed on frame id; S1 measures |
| AddMovementInput ignored in some S2 states | fallback: bot WASD + relative mouse |
| BB input only per scePadRead (~RT fps) | controller tested at 11 Hz; navigation runs in stock render mode when allowed |
| BB camera proxy metres off (wall pull-in, lock-on) | S3 player struct before metrics; proxy only fallback with loose stuck_dist |
| HUD HP needs capture | memory HP first; HUD probe only fallback, damage events from memory |
