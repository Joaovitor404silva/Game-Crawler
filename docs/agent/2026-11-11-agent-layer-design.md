# Agent Layer: design (rev 3, as built 2026-11-11)

Rev 3 changes (from building and live tests): one agentd on port 27070 that finds the game by scanning for an
AgentLink mapping; pad hosts get sticks computed by agentd (camera-relative, profile right_sign + look curve);
positions in metres with the host's own up axis (`up_axis` in the header); profiles are `.ini`; `agent_look` is C++ in
agentd (Windows.Graphics.Capture, desktop duplication fallback), so play is headless: the user keeps the PC.

Goal: AI agents see and control a running game in few, fast tool calls instead of screenshot -> guess -> key-press loops.
Scope v1: engine-neutral core + 2 hosts (Unreal via Game Crawler plugin, one external native host over AgentLink).
Jobs: mod QA/repro, autonomous play, visual review.

## Why the current loop is slow

| Today | Cost |
|---|---|
| 1 LLM turn per step (grab -> think -> key/pad hold) | 3-20 s per step; the game moves on meanwhile |
| Direction guessed from pixels | wrong turns, overshoot, no distance sense |
| No stuck detection | agent pushes into walls for many turns |

Prior art ([VideoGameBench], [game-player-mcp], Gemini/Claude Plays Pokemon): pixels-only agents finish ~0.5% of games;
what works is **state read from RAM + engineered movement/pathfinding tools, LLM out of the reflex loop**.

**Design rule:** one tool call = one whole intent ("go to X", "wait until Y"). A local controller runs the reflex loop at
the host's input rate; the LLM only plans and reacts to returned events.

## Architecture

```
 LLM --MCP--> mcp/server.js  agent_* tools (text + image content)
                   | TCP line protocol, 127.0.0.1:27070
              agentd.exe  thin shell: link, TCP, events, flight log
                   |  calls agentcore.h  step(State, Goal, Mem) -> Command   (pure, header-only, unit tested)
                   | AgentLink shared memory  Local\AgentLink_<pid>
       +-----------+-------------------------+
   UE host: plugin "agent"                    external host (any native runtime that maps AgentLink)
```

- **agentcore.h** is pure: no IO, no threads, no clock (dt comes in State). Tests and the sim call it directly.
- **agentd** is started on demand by `mcp/server.js` (first `agent_*` call). It takes `--pid` or picks the first process
  with an AgentLink mapping, binds 127.0.0.1:27070 exclusively (so never two controllers), exits when the game exits,
  and writes a 10 Hz flight log to `agent\logs\<game>-<time>.jsonl`.
- **Headless:** input goes through AgentLink into the game's own input path (no OS keyboard/mouse, no focus needed);
  `look` captures the game window's own frames even behind other windows. Not minimized windows. Never use the
  crawler's `bot focus on` for agent runs: the game then believes it is foreground and grabs the user's mouse.
  STALKER 2 runs at full rate unfocused (measured 60 fps) as long as no plugin does heavy work per tick.
- **Why shared memory:** external hosts may have no socket, and file polling is throttled and lossy. UE uses the same ABI so
  one controller serves both. Hosts stay dumb: publish state, apply the latest command.

## AgentLink v1 (shared memory ABI)

One 64 KiB mapping, little endian, floats, metres, degrees, host axes (`up_axis` 1 = Y up, 2 = Z up). **Each block has its own seqlock
`seq` (odd while writing) and exactly one writer thread.**

| Block | Writer | Content |
|---|---|---|
| header | host, once | magic `AGL1`, version, host kind, pid, block offsets |
| `frame` | host frame thread | frame_id, dt_ms, host time ms |
| `player` | host | pos[3], vel[3], yaw, pitch, hp, hp_max, stamina, area id + name[32], flags (alive, in_menu, in_dialog, loading, grounded, locked_on), valid-mask |
| `camera` | host render thread | frame_id, pos[3], fwd[3], view[12], proj[4], viewport rect (letterbox), valid |
| `entities` | host | frame_id, count, up to 64 x {id, kind, pos[3], hp%, name[24]} |
| `command` | agentd | cmd_seq, ttl_ms, move[3] (world direction x magnitude 0..1), look[2] (target yaw/pitch, used only by face/review), buttons bitmask, triggers[2], teleport pos[3] + flag |

Rules: a missing field is invalid in the mask, never faked. The host zeroes input when `cmd` is older than `ttl_ms`
(agentd crash = player stops; checked live on both the sim and Bloodborne). Pad hosts (caps STICKS) take `stick[4]` +
`buttons` that agentd computes from the camera block; world hosts (caps WORLD_MOVE) take `move` (UE plugin:
`SetMoveVector` when the pawn has it, else `AddMovementInput`), `look` and `teleport`. Stick calibration lives in
the profile, measured by `calibrate`, not in the host.

## Controller (agentcore)

- **Steering by move vector:** direction to the next waypoint, magnitude ramps down inside the slow radius, arrive within tol.
  Camera/look is only driven by `agent_face` and visual review.
- **Stuck:** progress toward the waypoint below `stuck_dist` (0.3 m) over a rolling `stuck_ms` (1.2 s) -> ladder: back
  off 450 ms, sidestep (alternating sides) 800 ms, push + dodge on the 2nd try; after `max_unstuck` (2) -> `stuck`.
- **Brake/coast:** release inside `tol + brake * speed/run` when heading in; the reply waits until the pawn stops.
- **Events (edge triggered):** damaged, hp<x, enemy within r, area changed, menu/dialog, died, arrived, stuck, idle.
  Every long action takes `stop_on=[...]` and returns the event that stopped it, plus the state text.
- **Bookmarks:** named positions per game in `agent\maps\<game>.txt` (`name x y z` lines).
- **Flight log:** frame/player/command at 10 Hz in JSONL per run, for diagnosis.
- v2 (only if live stuck rate > 10%): learned walk graph + A*, UE navmesh paths.

## MCP tools v1

`mcp/server.js` gains image content: handlers may return `{text, image:{mime,data}}` -> `content:[text,image]`
(images bypass the 12000-char text cap).

| Tool | Returns |
|---|---|
| `agent_state` | compact text: pos, heading, hp, area, nearest 8 entities with distance + bearing ("Huntsman 7.2 m, 30 deg left") |
| `agent_look` | annotated image (640 px) + the same text in one call |
| `agent_goto` target=xyz/bookmark/entity, tol, timeout, stop_on | arrived/stuck/event, time, distance walked |
| `agent_face` | target yaw/pitch or entity; done when aligned |
| `agent_do` | profile skill or timeline ("hold r1 120; wait 300; tap circle") |
| `agent_wait` until, timeout | the event or timeout + state |
| `agent_mark` | save/list bookmarks |
| `agent_tp` | teleport to bookmark/xyz when the host supports it |

Visual review in v1 = `agent_goto` + `agent_face` + `agent_look` per checkpoint (no route tool yet).

## Vision (agent_look)

Built in agentd (C++, no Python): Windows.Graphics.Capture of the game window (`src/agent/wgc.cpp`, works behind other
windows; DXGI desktop duplication fallback in `look.h`), crop to the client area, box downscale, GDI overlay (header
with pos/facing, labels at the pivot, marks and entities projected with view+proj), WIC JPEG. 16-20 ms warm.
`save=<name>` keeps the frame as evidence.

**Vision signals (`tools/agent_vision.py`), two backends behind one interface, chosen by measurement:**

| Signal | CPU backend | GPU backend |
|---|---|---|
| stuck / moving | OpenCV optical flow on downscaled frames | same flow on CUDA, or depth-change |
| HUD HP/stamina | HSV bar fill ratio (profile region) | n/a (CPU only) |
| prompts / menus | RapidOCR (ONNX CPU) + template match | OCR on CUDA |
| entities (no memory) | YOLO-World/YOLOE ONNX on CPU | same model on CUDA |
| free space ahead | none | Depth Anything V2 small |
| capture | windows-capture (WGC, unfocused ok) vs dxcam | same |

The bake-off picks per signal: accuracy over a threshold first, then the lower cost to the game. On a tie, CPU wins
(it leaves the GPU to the game and the path tracer). Memory state always beats vision when the field exists.

**Outcome (`vision-bakeoff.md`):** built `hud` (numpy, 0.2 ms, error 1.2%, fallback only) and `prompts` (RapidOCR on
DirectML, on demand: ~0.7 s, no FPS cost; CPU OCR cost 20% FPS). Stuck, free space and the entity detector were cut
or held: positions and memory entities already cover them.

## Game profiles (`agent\profiles\<game>.ini`)

`key = value` lines: run_speed, brake, start_lag_ms, slow_radius, min_stick, pass_radius, stuck_dist, stuck_ms,
max_unstuck, turn_rate, align_deg, look_dz, look_exp, right_sign, dodge (button bit), hp_low, enemy_r. `calibrate`
measures and rewrites run_speed, brake, start_lag_ms, right_sign, turn_rate. UE button -> key mapping is set in the
plugin (`agent keys cross=space ...`).

## External host

The first external host is a private native runtime; its design lives in that repo (`docs/AGENT_HOST.md`), not here.

## Gameplay agent (Claude Code subagent)

A custom subagent `gameplay` does all in-game driving, so the main session never spends its own context on frames.

- **Source:** `agents/gameplay.md` in this repo (generic); installed by copying into a game project's `.claude/agents/`
  (e.g. `<game dir>\.claude\agents\`). Prompt <= 5k tokens; game tips live in profile notes.
- **Tools:** `agent_*` MCP tools, read-only Game Crawler tools (`scene`, `find`, `get`), `Read`. No Edit/Write/Bash:
  it plays, it never changes code or files except its notes file.
- **Brief in:** goal (repro steps / reach state / explore / review checkpoints), success condition, budget (minutes,
  calls), stop rules (e.g. stop_on damaged).
- **Report out (short):** outcome reached/blocked/budget, what happened per step, final state text, evidence paths
  (saved `agent_look` images, flight log), new bookmarks. Facts only: it never gives PASS/FAIL verdicts.
- **Playbook in the prompt:** prefer `agent_goto` with `stop_on` over many small moves; on `stuck` try face + sidestep
  once, then report; one `agent_look` when state text is not enough; save bookmarks at every reached place.
- **Learning:** appends lessons to `agent\notes\<game>.md` (stuck spots, skills that work, menus); reads it first.
- **Rules:** one gameplay agent per game (agentd lock); on S2 only the supervisor dispatches it and owns live verdicts;
  live runs invincible.
- **Model:** opus medium by default; Phase 5 A/B vs sonnet on the metric task (turn latency matters), keep the winner.

## Non-goals v1

Vision-action models (NitroGen/Lumine later for vision-only games), learned nav graph, route record/replay, multiplayer,
anti-cheat games, generic OCR.

## Metrics (before/after, logged in ~/.claude/improvements.md)

Per game, one task ("reach bookmark ~50 m away with obstacles, then face nearest enemy"), 3 runs each, baseline = today's
tools. Measure: wall time, MCP calls, LLM tokens, success, plus host tick Hz and command-to-motion latency (frames).
Targets: <= 2 calls, >= 90% success, stuck rate < 10%. Keep a feature only where data beats the baseline.

## Quality

Done = agent moves like a player (no wall hugging, spinning or overshoot), works unfocused, input released on agentd
exit/crash (ttl) live, game behaves stock when agentd is off. Live runs are invincible.

[VideoGameBench]: https://opencv.org/videogamebench/
[game-player-mcp]: https://github.com/MrPauk335/game-player-mcp
