# Agent layer: live spike results

## Bloodborne (bbport RT, DLSS, 2026-11-11)

Host: AgentLink in bbport (`gpu/shim/bbport_agent.cpp`), pad merge in `sample()`, camera from the RT camera constants,
player = the game camera's **pivot** (+0xD0 of the camera object, read in the camera-update hook).

| Measure | Value |
|---|---|
| camera block rate | 49 Hz (RT path) |
| pad reads (scePadRead) | 48.5 Hz |
| player (pivot) updates | 74 Hz |
| command written -> applied by pad | ~14 ms |
| stick -> first motion | 206-234 ms (start animation) |
| stick released -> stopped | 400-443 ms, overrun 0.6-0.8 m |
| run speed, full stick | 3.32 m/s |
| move direction | camera relative: cos(move, camera fwd) = +1.00 forward, -1.00 back |
| turn, rx = +1 | ~125 deg/s camera yaw |
| ttl expiry | input stops (drift 0.004 m) |
| half stick (0.5) | walk; blocked runs show 0.0-0.2 m (stuck detected from pivot) |

Findings that changed the design:
- **Camera position is a bad player proxy** (lagging spring camera, auto-rotation): movement showed up late and
  sideways. The camera pivot is a good one. Player block = pivot (`AGL_V_PROXY`), lock-on flag from +0x154.
- **Hook availability:** the camera hook was only installed in mouse direct mode with a captured mouse; split into
  `SetProbe` (jump only) + direct mode (NOPs), so agents work unfocused.
- **Pad hosts get sticks, not world move:** agentd converts world direction to camera-relative stick
  (it has the camera block). Host stays dumb. `move[]` is only for `AGL_CAP_WORLD_MOVE` hosts (UE).
- **Coordinates:** Y up (`up_axis = 1`), metres.
- Controller must brake ~0.7 m before the target and expect ~0.2 s start lag.

## S2 (Unreal)

Pending: measured at T12 (pump vs frame rate, AddMovementInput magnitudes). The core does not depend on it.
