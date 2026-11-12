# Metric tasks per game

Same task before/after any agent-layer change; briefs in `gameplay-evals.md`. Live runs are invincible.

## Bloodborne (bbport)

- **E1 (metric):** stand on mark `e1_start` in the lower alley below Central Yharnam (-123.8, -78.3, -34.2, facing
  ~33 deg), walk to a visible point ~15 m out, mark `e1_goal`, return, face it. Success: <= 1.5 m, <= 10 deg.
  Reset between runs: `agent_goto to=e1_start tol=0.4`, `agent_face yaw=33`, `agent_mark action=remove name=e1_goal`.
  The corridor stops forward walks at ~12 m (arch); that is part of the task (agent must handle `stuck`).
- **E2 (QA repro):** at `e1_start`, `tap r1; wait 400; tap r1` (two light attacks), look, report distance moved.
- **E3 (visual review):** marks `e1_start`, `e1_goal`, `lower_alley`; face the arch / corridor / stairs, one image each.
- **Combat (long):** find the nearest enemy from `agent_state`, kill it with lock-on (`tap r3`) + r1, confirm by the
  entity leaving the list, return to `e1_start`.

## STALKER 2 (parked)

Bookmark ~50 m out + nearest enemy, same success rules. Not measured yet.
