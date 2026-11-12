# Agent layer

Lets an AI agent see and drive a running game in a few fast MCP calls: "go to X", "face Y", "look", instead of
screenshot -> guess -> key loops. Design: [2026-11-11-agent-layer-design.md](2026-11-11-agent-layer-design.md).

## Parts

| Part | Where | Job |
|---|---|---|
| AgentLink | `src/agent/agentlink.h` | shared memory `Local\AgentLink_<pid>`: game state out, commands in |
| agentcore | `src/agent/agentcore.h` | pure controller: goto, face, stuck ladder, events |
| walk graph | `src/agent/walkgraph.h` | learned routes from walked positions, A* for goto |
| agentd | `src/agent/agentd.cpp` -> `bin\agentd.exe` | loop + TCP 127.0.0.1:27070 + look + flight log |
| UE host | `plugins/agent/agent.cpp` | Game Crawler plugin that publishes the pawn/camera and applies commands |
| MCP tools | `mcp/agent.js` | `agent_*` tools; starts agentd on first use |
| gameplay subagent | `agents/gameplay.md` | Claude Code subagent that does the driving |

Any native runtime can be a host: map `Local\AgentLink_<pid>` (see `agentlink.h`), publish frame/player/camera,
apply `command` while `now - cmd time < ttl_ms`.

## Tools

| Tool | Does |
|---|---|
| `agent_state` | pos, facing, hp, flags, nearest entities |
| `agent_goto` to=x,y,z / mark / #entity, or fwd/right metres; tol, timeout, stop_on, path | walks there; arrived / stuck / wrong_level (target on another floor, height > profile `level_tol`) / event / timeout. A single `to` follows the walk graph when known ground joins both ends (`path=0`: straight) |
| `agent_face` yaw / to / entity | turns the camera |
| `agent_do` steps | timeline, e.g. `hold cross 200; wait 300; tap circle` |
| `agent_wait` until, timeout | waits for an event |
| `agent_look` width, overlay, save | game frame (image) + state text |
| `agent_mark` set / list / remove | named positions per game |
| `agent_calibrate` | measures run speed, brake, start lag, stick side, turn rate into the profile |
| `agent_tp` to | teleport (hosts with TELEPORT) |
| `agent_stop` | releases all input |

## Headless (you keep using the PC)

- Input never goes through the OS keyboard/mouse: AgentLink -> host -> the game's own input path. No focus needed.
- `agent_look` uses Windows.Graphics.Capture on the game window: works behind other windows. Not when minimized.
- Never `bot focus on` during agent runs: the game thinks it is foreground and takes the user's mouse. Not needed:
  the UE host calls the pawn directly (`SetMoveVector`, `SetControlRotation`), STALKER 2 measured 60 fps unfocused.
- Host plugins: never `api->find` per tick (a full object scan, ~100 ms on STALKER 2 = 5 fps). Cache the controller,
  read the pawn from `pc.Pawn`, and keep crawler calls outside AgentLink seqlocks.
- If agentd dies, the host drops the input after `ttl_ms` (200 ms): the player stops.
- A game takes the foreground once when its window opens; click back to your work, everything after (menus via
  `agent_do` taps included) runs unfocused. Checked live 2026-11-11 on Bloodborne and STALKER 2.

## Walk graph

`src/agent/walkgraph.h`: agentd records every position the player really walks (any command) into a graph per game,
`agent\maps\<game>.graph`. Nodes every 1.5 m of track; a track passing within 0.9 m of a node joins it, so crossing
tracks connect. Edges follow the walk; an edge is walkable backwards when it rises at most 1.2 m and at most 45 deg
(stairs both ways, drops one way). Teleports and pauses over 1 s make no edge. With no graph file, agentd learns
one from the game's old flight logs on start. A goto to one target plans (Dijkstra) from every node within 3 m of the
start (height 1.5 m) to every node within 5 m of the target (height up to `level_tol`), so a dead-end or duplicate
track near either end does not block the route; the result says "via walk graph (N waypoints)" or
"straight (no walked route known)". The agentd command `graph` prints the size.

When a planned goto ends `stuck`, agentd drops the graph edges along the stuck leg near the stuck spot. The result
adds ", forgot N walk-graph edge(s)", so a door that closed later is planned around next time, provided another
walked route exists.

Limits: tracks that run parallel but never cross stay apart, even when they are only 3 m apart, so a plan can make a
detour. Joining them was tried and dropped, because a fence between two tracks would get joined along its whole
length. `path=0` forces a straight walk.

## Files

- `agent\profiles\<game>.ini`: `key = value` tuning (run_speed, brake, start_lag_ms, turn_rate, right_sign, look_dz,
  look_exp, dodge, ...). `agent_calibrate` fills it.
- `agent\maps\<game>.txt`: marks; `agent\maps\<game>.graph`: walk graph (`v x y z` nodes, `e a b` edges). `agent\logs\`: flight logs (JSONL, 10 Hz) and saved looks. `agent\notes\<game>.md`:
  lessons written by the gameplay subagent. Logs, maps and notes are git-ignored.

## Vision signals (optional)

`tools/agent_vision.py` for hosts that cannot read a field from memory: `hud` (bar fill of HP/stamina, regions
`hud_hp`/`hud_stamina` in the profile, 0.2 ms) and `prompts` (RapidOCR text, `prompt_region`, GPU via DirectML, ~0.7 s,
on demand). Setup and numbers: [vision-bakeoff.md](vision-bakeoff.md). Memory always wins when the host has the field.
`tools/agent_vision_dataset.py` records auto-labelled frames (labels from memory) and scores the HUD reader;
`tools/agent_stats.py` summarizes goto outcomes (stuck rate) from the flight logs' result rows.
agentd serves several clients at once (MCP + scripts); commands still run one at a time.

## Install

- Build: `src\build.bat agentd` (or a full `src\build.bat`).
- UE games: the `agent` plugin loads with Game Crawler; `agent status` shows the link. `AGENTLINK=0` turns it off.
- Subagent: `powershell -File agents\install.ps1 -Project <game folder>` copies `gameplay.md` into
  `<game folder>\.claude\agents\`.
- MCP: the tools come with the Game Crawler MCP server; reconnect it (`/mcp`) after an update.

## Tests

`tests\run_tests.bat` runs the agentcore unit tests, `tests\agent\test_agentd.js` (agentd against a simulated host)
and, when `tools\.venv-vision` exists, `tests\agent\test_vision.py`.
