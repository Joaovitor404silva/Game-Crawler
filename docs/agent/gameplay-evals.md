# Gameplay agent evals

Three briefs to dispatch the `gameplay` subagent with (one game at a time, live runs invincible).
Pass = the report matches the flight log (`agent/logs/<game>-*.jsonl`) and saved images: no invented positions,
events or results. Record calls, wall time and outcome per run in `baseline.md` (Phase 5 table).

## E1 metric task (the Phase 0 task)

> Goal: from the current spot, walk to a point about 15 m away that you can see, mark it `e1_goal`, walk back to
> the start mark `e1_start`, then face the goal. Success: `agent_state` within 1.5 m of `e1_start` and facing
> within 10 deg of `e1_goal`. Budget: 12 calls, 3 minutes. Stop on: died.

Before dispatch: `agent_mark name=e1_start` at the spawn. Baseline (screenshots + raw pad): 6 calls, 25 s, lost.

## E2 QA repro

> Goal: go to mark `<place>`, do `<skill timeline>` (e.g. `tap r1; wait 400; tap r1`), take `agent_look
> save=e2_after`. Report the distance moved by the skill and anything the image shows near the player.
> Budget: 8 calls, 3 minutes. Stop on: damaged, died.

## E3 visual review

> Goal: visit marks `<a>`, `<b>`, `<c>` in order; at each, face `<subject>` and `agent_look save=e3_<mark>`.
> Report the three image paths and the state at each. Budget: 15 calls, 5 minutes.

## Scoring

| run | model | calls | wall s | outcome | report = log? |
|---|---|---|---|---|---|
| BB E1 A/B 1 (lower alley) | opus medium | 7 | 36 | reached, 0.8 m, 5 deg | yes |
| BB E1 A/B 2 | sonnet medium | 7 | 34 | reached, 1.0 m, 6 deg | yes; re-set `e1_start` at its own spot (0.5 m) |
| BB E1 A/B 3 | opus medium | 8 | 36 | reached, 0.8 m, 6 deg | yes |
| BB E1 A/B 4 | sonnet medium | 8 | 30 | reached, 0.9 m, 6 deg | yes; re-set `e1_start` |
| BB E1 A/B 5 | opus medium | 9 | 47 | reached, 0.8 m, 6 deg | yes |
| BB E1 A/B 6 | sonnet medium | 9 | 30 | reached, 1.0 m, 6 deg | yes; re-set `e1_start` |
| BB E1 verify (playbook: never re-set existing marks) | sonnet medium | 7 | 31 | reached, 1.0 m, 6 deg | yes; kept `e1_start` |
| BB E2 QA repro (e1_start, r1 x2) | sonnet medium | 3 | 14 | done: skill moved 1.66 m, image described correctly | yes; skipped reading the notes file |
| BB E3 visual review (3 marks) | sonnet medium | 11 | 35 | done, 3 images | yes; 3rd image blocked by the hunter (camera turned, body did not) -> playbook: step 1 m after facing; look now returns the saved path |
| BB combat from e1_start, no route in notes | sonnet medium | 16 | 84 | **blocked**: only straight gotos at enemies behind walls, gave up with 29 calls left; read a note as "no enemies here" | yes |
| BB combat from e1_start, same brief + notes | opus medium | ~27 | 139 | kills 3 crawlers (echoes 8796 -> 9000, image), found stairs, marked the route, back 1.0 m | yes, except an invented flight-log file name |
| BB combat from e1_start, same brief + route notes (written by the opus run) | sonnet medium | 13 | 65 | kills 3 crawlers (412/409/410 left the list, echoes 9000 -> 9204, image), back 1.0-1.2 m | yes; evidence paths vague (no saved path quoted) |
| BB combat from e1_start, same brief, notes hold the place but no route; walk graph on | sonnet medium | 14 | 77 | kill 268517408 (echoes 9204 -> 9272, image checked), plain gotos via walk graph both ways (18-19 s each), back 1.0 m | yes; quoted the exact saved paths; 1st try (an entity on another floor) stuck, dropped per brief |
| BB combat 2026-11-11 (find + kill an enemy, return) | opus medium | 41 | 173 | kills done (crows, lower alley; echoes 7885 -> 8056 -> 8124, images match); return blocked: start is 19.6 m up, no route found | yes; found the goto height bug -> `wrong_level` status, verified live |
