# Agent layer: baseline (today's tools)

## Bloodborne (bbport RT, DLSS, ~77 FPS), boot spot = save start in front of the archway

| Run | Tools | Calls | Wall | Outcome |
|---|---|---|---|---|
| 1 (2026-11-11) | bbrt `pad_hold` + `vision_grab` | 6 (3 move, 3 grab) | 25 s tool time (+ LLM turns) | **lost**: corridor past the arch is near black; after 5.5 s forward + a 45 deg turn the frame shows only a close wall/statue. No way to tell position, progress or whether the walk was blocked. |
| 2 (2026-11-12, e1_start, same task: 15 m out, back, face the goal) | same | 8 (4 move, 4 grab) | 21 s tool time | **wrong, unseen**: rx turns the camera, not the body, so the 180 deg "turn" plus walk back ended against a wall. A second 180 deg turn left the camera facing the character's front. Judge (`agent_state`): 3.3 m off the start; body yaw 4 deg off. The next "forward" would go the wrong way. |
| 3 (2026-11-12, by the arch; r3 recenter, forward 4.5 s, back 4.5 s, no turn) | same | 6 (3 move, 3 grab) | 10 s tool time | **wrong, unseen**: the forward leg vanished into a black tunnel, so the distance can't be checked. The back leg turned the character toward the camera. Judge: 4.0 m off the start; the frame "looks right" (arch in view), so the agent would report done. |

Runs 1-3: none finished the task the agent layer did in 7 calls (E1 below). Two runs ended 3-4 m off without any
signal, and every run lost track of position in a dark section.

Observed failure modes: frames are unreadable in dark areas, no distance sense, no stuck signal, each step costs one
LLM turn plus 0.3 s pad-file settle.

## After: agent layer (agentd + agent_* MCP tools)

| Run | Tools | Calls | Wall | Outcome |
|---|---|---|---|---|
| E1 (2026-11-11, street by the stairs) | mark, look, goto fwd=15, mark, goto start, face goal, look | 7 | 15.3 s tool time | **done**: forward walk reported `stuck` 3.5 m short (iron fence at ~12 m, correct), back to start 0.8 m off, facing 5 deg off the goal. |

Live numbers (BB): `goto` 6 m 2.2 s / diagonals 2.1-2.6 s, end 0.5-0.8 m; `face` 90-180 deg in 1.2-1.8 s, within
5 deg; `look` 16-20 ms warm (200 ms first call); agentd killed mid-run -> player stops (0 drift over 0.5 s).

### Gameplay subagent A/B (Phase 5, 2026-11-11, E1 in the lower alley, 3 runs each, same brief)

| Model | Success | Game calls | Wall s (avg) | Tokens (avg) | Slips |
|---|---|---|---|---|---|
| opus medium | 3/3 | 7-9 | 39.5 | 57.6k | none |
| sonnet medium | 3/3 | 7-9 | 31.4 (-21%) | 56.9k | re-set the brief's start mark 3/3 |

Kept: sonnet (faster, cheaper per token, same result) with a playbook line against re-setting existing marks.
Long task (combat from e1_start, 3 crawlers up a staircase, see `gameplay-evals.md`):

| Model | Notes hold the route? | Calls | Wall s | Outcome |
|---|---|---|---|---|
| sonnet medium | no | 16 | 84 | blocked (straight gotos at walled targets) |
| opus medium | no | ~27 | 139 | 3 kills, found the stairs, wrote the route |
| sonnet medium | yes | 13 | 65 | 3 kills, back at start |
| sonnet medium | place only, walk graph on | 14 | 77 | 1 kill (brief asks 1), plain goto there and back via the graph |

Rule: sonnet by default; opus (`model: opus` on dispatch) for first exploration of an area with no route in the notes.

### Stuck rate (goto result rows, `tools/agent_stats.py`)

14 gotos in the combat runs: arrived 5, event 1, stuck 8 -> raw stuck rate 57%. `stuck_short` (stuck within 3 m of
the target) = 0: every stuck was a straight goto at a target behind a wall or on another floor. The controller
did not fail on reachable targets; the agent lacked a route. Fix in use: routes in the notes + the playbook line
"target behind a wall = waypoints".

### Walk graph (2026-11-12, `src/agent/walkgraph.h`)

| Trip (65 m, a staircase, walls) | Before | After |
|---|---|---|
| pack -> e1_start | sonnet: blocked; opus: ~27 calls exploring | 1 goto, 18.4 s, 0 unstuck (graph learned from old logs) |
| e1_start -> pack | waypoints from notes | 1 goto, 17.1 s, 0 unstuck |
| stairs bottom -> entity #id | straight goto stuck (16 s) | 1 goto, 25.3 s, 97 m (detour: start on a dead-end track) |
| stairs bottom -> pack, after multi-source planning | 97 m detour | 1 goto, 14.4 s, 49 m |
| e1_goal -> e1_start (street logged twice at two pivot heights) | straight (no plan) | via graph, 4.2 s |

Routes no longer need to live in the notes; notes keep places.

| Metric | Before | After |
|---|---|---|
| Calls for E1 | 6, unfinished | 7, finished |
| Position / progress known | no | yes (pivot, every call) |
| Stuck signal | none | `stuck` + unstuck ladder, 10.5 s worst case |
| Image cost | every step | 2 per task (optional) |
