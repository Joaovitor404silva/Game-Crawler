# Anomaly-Scanner

A live, in-process debugger for **S.T.A.L.K.E.R. 2: Heart of Chornobyl** (UE 5.5, Steam, Windows), built for modders and
for AI coding agents. It runs inside a patched *copy* of the game and exposes Unreal reflection, memory, hooks and hardware
breakpoints over a local TCP port. It comes with a CLI and an **MCP server**, so Claude Code (or any MCP client) can inspect
and drive the running game.

- Find UObjects, read and write properties, call UFunctions on the game thread
- Read and write memory, evaluate pointer chains, resolve symbols from a names database
- Inline hooks and hardware breakpoints / write watches, with caller stacks and aggregated hits
- Typed `TArray` / `TSet` / `TMap` dumps, actor components, bone poses, NPC census, live quest graph
- C++ **plugins** that hot reload on save, and hot reload of the debugger core itself without restarting the game
- 49 MCP tools; the offline tools (`names`, `decomp`, `log`, `hits`) also work without the game running

> **Single-player dev tool.** It never modifies your Steam install. It builds a separate dev folder made of links to the game
> files plus a patched copy of the exe. Tested on game build **26123434** (Update 2.1). Other builds may need new signatures
> or offsets. Unofficial, single-player only, use at your own risk; read the [Disclaimer](#disclaimer).

---

## How it works

| Piece | Role |
|---|---|
| `src/patch_exe.js` | Copies the game exe and adds a `.s2dev` section with a rebuilt import directory, so the game loads `S2Dev.dll!S2DevInit` at start. It strips the signature, recomputes the checksum, verifies the result, and refuses to write into `steamapps`. |
| `src/make_devroot.ps1` | Builds the dev folder (default `C:\S2DevRoot`): junctions and hardlinks to the Steam install, the patched exe, no UE4SS, no mod paks. Idempotent. |
| `S2Dev.dll` (`src/loader.cpp`) | Loader: log, TCP server on `127.0.0.1:27060`, `GameEngine::Tick` hook (game-thread pump), hot reload of the core. |
| `S2DevCore.dll` (`src/core.cpp`, `ue.cpp`, `hooks.cpp`, `probe.cpp`, `plugins.cpp`) | Every command. Loaded from a shadow copy, so it can be rebuilt and reloaded while the game runs. |
| `anomaly.js` | CLI client. |
| `mcp/server.js` | MCP server (stdio, JSON-RPC 2.0, no dependencies). |
| `plugins/` | Plugin API (`s2dev_api.h`), `example` (hooks one game function) and `nbot` (a scripted player bot). |

## Requirements

- Windows 10/11 x64, STALKER 2 (Steam)
- Visual Studio 2022 (any edition) with the **MSVC x64 C++ tools**; the build finds it through `vswhere`
- Node.js 18+
- PowerShell 5.1+ (the dev folder uses junctions and hardlinks, so it must be on the same drive as the game for hardlinks)

## Setup

```powershell
git clone --recursive <this repo>        # MinHook is a submodule (src/minhook)
cd Anomaly-Scanner
cmd /c src\build.bat                       # -> bin\S2Dev.dll, bin\S2DevCore.dll
powershell -File src\make_devroot.ps1      # once; -Game <game folder> -DevRoot <dev folder> to override
powershell -File launch.ps1                # starts the dev game windowed, saves in Saved_dev
node anomaly.js ping                         # -> pong pid=... core=loaded
```

`launch.ps1` sets `ANOMALY_HOME` to this folder, so the DLLs find `bin\`, `plugins\`, the logs and the optional names database
here. It refuses to start a second dev game (`-Force` launches alongside other STALKER 2 instances).

### Register the MCP server

```powershell
claude mcp add anomaly-scanner -- node <path>\Anomaly-Scanner\mcp\server.js
```

## CLI

`node anomaly.js [--json] [--port N] <command>`, `-i` interactive, `-f file` script.

```text
ping
find class:Stalker2PlayerController
get pawn BaseEyeHeight
set pawn CharacterMovement.MaxWalkSpeed 900
call pawn K2_GetActorLocation
hook Agent::IsChangingWeapon log        # then: log 20
bpw 0x13a8073d040 8               # who writes these 8 bytes (hits in the log), then: bpc all
con "t.MaxFPS 30"
```

Main command groups:

- **loader:** `ping`, `reload` (after `src\build.bat core`), `unload`
- **memory:** `rd <addr> [n]`, `wr <addr> <hex>`, `ptr <expr>`, `aob <pattern>`, `rva`, `va`, `mods`, `sym <addr>`
- **reflection:** `find <substr|class:Name|isa:Name> [near=<obj|x,y,z>] [r=<cm>] [loc]`, `obj`, `get`, `set`, `call`, `funcs`, `props`,
  `classes`, `watchprop` / `unwatch`, `con <console command>`, `cvar <name> [value]`, `uinfo`
- **hooks and breakpoints:** `native <name>`, `hook <addr|name> [log|block|ret <v>] [hw] [force]`, `unhook`, `hooks`,
  `bpx` / `bpw` / `bprw` / `bpc` / `bps`, `watchw <addr> <len> secs=<n>` (who writes this field), `agg` (hits grouped by stack)
- **game state:** `scene` (preflight), `npcs`, `uid`, `comps <actor>`, `pose <actor>`, `quest list|state|tap|layout`,
  `tarray|tset|tmap <obj> <Prop.Path>`, `layoutcheck <Class>`
- **capture:** `snap` (typed memory fixture -> `snaps\<name>.json`), `sample` (in-core sampler), `memdiff` (which bytes change), `stats`
- **misc:** `xcall <addr|Name> [rcx rdx r8 r9]`, `mark`, `log [n]`, `plug list|build|reload|unload <name>`

## MCP tools

`status, find, obj, get, set, call, funcs, props, classes, rd, wr, ptr, sym, addr, names, decomp, native, hook, unhook, hooks, bp,
bpc, bps, hits, watchw, agg, scene, snap, sample, npcs, uid, comps, pose, quest, tarray, tset, tmap, layoutcheck, memdiff, stats,
batch, mark, watch, unwatch, plugin, con, log, reload_core, raw`

Every tool takes an optional `game` (port or alias) to target one of several dev games, and `all` runs it on every port.
Results are capped at 12000 characters (`ANOMALY_MCP_CAP`).

## Plugins

`plugins\<name>\*.cpp` -> `bin\plugins\<name>.dll` (`plugins\build_plugin.bat <name>`). The core polls the folder every second. On
save it rebuilds (5-15 s) and swaps the plugin on the game thread: the old `S2PluginShutdown` runs, its hooks are removed, the
fresh copy is loaded and `S2PluginInit` runs. A failed build keeps the old copy running.

```cpp
#include "s2dev_api.h"
static const S2Api* api;
typedef bool(__fastcall* Fn)(void*); static Fn orig;
static bool __fastcall Detour(void* self) { return true; }
S2_EXPORT int S2PluginInit(const S2Api* a) { api = a; return api->hook(api->addr("Agent::IsChangingWeapon"), (void*)Detour, (void**)&orig); }
S2_EXPORT void S2PluginTick() {}
S2_EXPORT void S2PluginShutdown() {}
```

API (`plugins/s2dev_api.h`, version 1):

| Member | Meaning |
|---|---|
| `base` | Game exe base address |
| `log(fmt, ...)` | Append to `s2dev.log`, prefixed with the plugin name |
| `addr(expr)` | Address of a names-database name (`Agent::IsChangingWeapon`), a native, or any S2Dev expression; 0 = not found |
| `sym(a, buf, n)` | Address -> `exe+0x.. Name+0x..` |
| `hook` / `unhook` | Inline hook (returns 1 on success); hooks are removed automatically on unload |
| `find(obj)` | UObject by `pawn`, `pc`, name, path or `0x` address |
| `get` / `set` / `call` | Property as text / write a property / call a UFunction (queued to the game thread, up to 5 s) |
| `cmd(line, out, n)` | Run any S2Dev command |
| `ticks()` | Engine tick counter |

Exports: `S2PluginInit` (required, return 1 = ok), `S2PluginTick` (optional, game thread, every tick), `S2PluginShutdown`
(optional). A plugin that starts threads must stop and join them in `S2PluginShutdown`. Wrap risky pointer reads in
`__try` / `__except`.

`nbot` is a scripted player bot (movement, scenes, quest goals) driven through files in `bench\io\<port>`. Set `ANOMALY_NBOT=0`
to keep it inert.

## Names database and decompile (optional)

`addr`, `sym`, `names` and plugins that resolve functions by name need a names database: `names\names.tsv`
(`rva<TAB>kind<TAB>name<TAB>source`, rva in hex from image base `0x140000000`). The `decomp` tool needs a Ghidra decompile dump with
an `index.tsv` (`rva, name, file, line, size, callers`) in `decomp\`. Neither is shipped. Both are generated from your own copy of
the game; this repo does not include game code or data.

Build the names database with the dev game running (main menu is enough):

```powershell
node tools\build_names.js                  # dumps UFunctions, vtables and FField vtables live, then writes names\names.tsv
node tools\build_names.js --ue4ss <path>\UE4SS.log   # optional: virtual slot names from a UE4SS log of the same game
node tools\build_names.js --offline        # rebuild from the raw dumps already in names\
```

It names native UFunction exec thunks and their implementations, vtables and their overridden slots, UClass constructors,
and the PE imports and exports of the exe (`--exe` if the game is not in the default Steam folder). It takes a few minutes and
must be rerun after a game update.

## Configuration

| Variable | Default | Used for |
|---|---|---|
| `ANOMALY_HOME` | `C:\s2dev` (set by `launch.ps1`) | Root for `bin\`, `plugins\`, logs, `names\`, `sdk\`, `bench\` |
| `ANOMALY_PORT` | `27060` | TCP port of the dev game (one per game when several run) |
| `ANOMALY_LOGDIR` | `ANOMALY_HOME` | Log folder (`s2dev.log`, `s2dev_<port>.log`) |
| `ANOMALY_CORE` | `bin\S2DevCore.dll` | Core DLL to load (staged builds) |
| `ANOMALY_NAMES` | `names\names.tsv` | Names database |
| `ANOMALY_NATIVES` | `names\natives_impl.tsv` | Native exec -> impl table |
| `ANOMALY_DECOMP` | `decomp` | Decompile dump folder (MCP `decomp`) |
| `ANOMALY_SDK` | `sdk` | Per-class SDK tables for `layoutcheck` |
| `ANOMALY_NBOT` | `1` | `0` disables the nbot plugin |
| `ANOMALY_TIMEOUT`, `ANOMALY_MCP_CAP`, `ANOMALY_ALL_PORTS` | | CLI/MCP timeout, MCP result cap, port list for `game=all` |

## Tests

```powershell
cmd /c tests\run_tests.bat                 # C++ units + core scenario in a test exe, MCP server and CLI against fake servers
cmd /c plugins\nbot\test\build.bat         # nbot math tests
```
No game needed.

## Notes

The UE 5.5 layout is detected live on first use (`uinfo`): FField name at 0x20, Offset_Internal at 0x44, sizeof(FProperty) = 0x70,
item stride 24. Early reflection commands answer `error: reflection not ready` and retry the signature scan.

## Disclaimer

- **Unofficial.** This project is not affiliated with, endorsed by or supported by GSC Game World, Microsoft, Valve or
  Epic Games. S.T.A.L.K.E.R. 2: Heart of Chornobyl is a trademark of GSC Game World. Unreal Engine is a trademark of Epic Games.
  All other trademarks belong to their owners.
- **No game content.** This repository contains no game code, assets, decompiled code or data from the game. The names
  database and the decompile are generated locally from your own legally obtained copy and must not be redistributed.
- **Single-player and offline use only.** Do not use it in online or multiplayer sessions, with anti-cheat, or to gain an
  advantage over other players. Doing so may break the game's terms of service and can get your account banned.
- **Use at your own risk.** It injects code into a running process and writes to its memory. It can crash the game, corrupt
  saves or cause data loss. Back up your saves (the dev game uses a separate `Saved_dev` folder, but nothing is guaranteed).
  The software is provided "as is", without warranty of any kind; see `LICENSE`.
- **Your responsibility.** You are responsible for complying with the game's EULA and the laws of your country. Modding and
  reverse engineering for interoperability are allowed in many places but not all.
- **Antivirus.** DLL injection, inline hooks and exe patching can trigger antivirus warnings. Build from source and check the
  code yourself if in doubt.
- **AI-assisted.** Parts of this project were written with the help of AI coding tools.

## License

MIT, see `LICENSE`. MinHook (`src/minhook`, submodule) is BSD-2-Clause, copyright Tsuda Kageyu.
