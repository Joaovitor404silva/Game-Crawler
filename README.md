# Game Crawler

A live, in-process debugger for **Unreal Engine games** (UE 4.25+ and UE 5.x, Windows x64), built for modders and for AI
coding agents. It loads into a dev *copy* of the game through a `version.dll` proxy and exposes Unreal reflection, memory,
hooks and hardware breakpoints over a local TCP port. It comes with a CLI and an **MCP server**, so Claude Code (or any MCP
client) can inspect and drive the running game.

- Find UObjects, read and write properties, call UFunctions on the game thread
- Read and write memory, evaluate pointer chains, resolve symbols from a names database
- Inline hooks and hardware breakpoints / write watches, with caller stacks and aggregated hits
- Typed `TArray` / `TSet` / `TMap` dumps, actor components, bone poses, SDK and function dumps
- C++ **plugins** that hot reload on save, and hot reload of the debugger core itself without restarting the game
- 46 MCP tools; the offline tools (`names`, `decomp`, `log`, `hits`) also work without the game running

No per-game signatures. On start it finds `GUObjectArray` and the `FNamePool` by scanning the exe's `.data` section, detects
the FField / UFunction layout from known engine classes, and finds the `ProcessEvent` vtable slot from `Default__Object`.

> **Single-player dev tool.** It never modifies your game install: it builds a separate dev folder made of links to the game
> files and puts the proxy there. Tested on S.T.A.L.K.E.R. 2 (UE 5.5), Backseat Drivers (UE 5), Deep Rock Galactic
> (UE 4.27) and Hogwarts Legacy (UE 4.27, DRM-wrapped exe); generic tools and the input bot also on S.T.A.L.K.E.R. Anomaly (X-Ray). Other engine versions and custom engine forks may need fixes. Unofficial, offline use only, at your own risk; read the [Disclaimer](#disclaimer).

---

## How it works

| Piece | Role |
|---|---|
| `src/make_devroot.ps1` | Builds the dev folder (default `C:\GameCrawlerRoot\<game>`): junctions and hardlinks to the install, the `version.dll` proxy in `Binaries\Win64`, no other proxy DLLs (UE4SS, dxgi, dinput8, ...), no `Mods`/`~mods`/`LogicMods`, no `Saved`, plus `steam_appid.txt` for Steam games so they start outside Steam's launcher. Idempotent. |
| `version.dll` (`src/core/proxy.cpp`) | Forwards every export to `System32\version.dll` and loads `bin\GameCrawler.dll` from the folder in `CRAWLER_HOME` or `crawler_home.txt`. |
| `GameCrawler.dll` (`src/core/loader.cpp`) | Loader: log, TCP server on `127.0.0.1:27060`, game-thread pump (hook on `PeekMessage` for any game, plus `ProcessEvent` on Unreal; main thread only), hot reload of the core. |
| `GameCrawlerCore.dll` (`src/core/*` + `src/engines/*`) | Every command. `src/core` is engine-neutral (memory, hooks, breakpoints, capture, plugins); `src/engines/<engine>/` holds one backend per engine (`ue` today) behind `src/engines/engine.h`; `engine` shows which one was detected. Loaded from a shadow copy, so it can be rebuilt and reloaded while the game runs. |
| `crawler.js` | CLI client. |
| `mcp/server.js` | MCP server (stdio, JSON-RPC 2.0, no dependencies). |
| `plugins/` | Plugin API (`crawler_api.h`) and `example`. |

## Requirements

- Windows 10/11 x64, a 64-bit game without anti-cheat (Unreal Engine for the engine tools; generic tools and the input bot work on any game)
- Visual Studio 2022 (any edition) with the **MSVC x64 C++ tools**; the build finds it through `vswhere`
- Node.js 18+
- PowerShell 5.1+ (hardlinks need the dev folder on the same drive as the game, otherwise files are copied)

## Setup

```powershell
git clone --recursive <this repo>        # MinHook is a submodule (src/minhook)
cd Game-Crawler
cmd /c src\build.bat                                         # -> bin\GameCrawler.dll, bin\GameCrawlerCore.dll, bin\proxy\version.dll
powershell -File src\make_devroot.ps1 -Exe "<game>\<Project>\Binaries\Win64\<Project>-Win64-Shipping.exe"
powershell -File launch.ps1 -DevRoot C:\GameCrawlerRoot\<game>     # windowed 1280x720, saves in Saved_dev
node crawler.js ping                                           # -> pong pid=... core=loaded
node crawler.js uinfo                                          # reflection layout, once the engine is up
```

Use the `*-Win64-Shipping.exe` in `Binaries\Win64`, not the small launcher exe in the game root. `launch.ps1` refuses to start
a second dev game; `-Force` launches it next to a normal copy, `-GameArgs` overrides the command line. Steam games may need
Steam running.

### Register the MCP server

```powershell
claude mcp add game-crawler -- node <path>\Game-Crawler\mcp\server.js
```

## CLI

`node crawler.js [--json] [--port N] <command>`, `-i` interactive, `-f file` script.

```text
ping
find class:PlayerController
get pawn BaseEyeHeight
set pawn CharacterMovement.MaxWalkSpeed 900
call pawn K2_GetActorLocation
dumpsdk sdk.tsv
bpw 0x13a8073d040 8               # who writes these 8 bytes (hits in the log), then: bpc all
con "t.MaxFPS 30"
```

Main command groups:

- **loader:** `ping`, `reload` (after `src\build.bat core`), `unload`
- **memory:** `rd <addr> [n]`, `wr <addr> <hex>`, `ptr <expr>`, `aob <pattern>`, `rva`, `va`, `mods`, `sym <addr>`
- **reflection:** `find <substr|class:Name|isa:Name> [near=<obj|x,y,z>] [r=<cm>] [loc]`, `obj`, `get`, `set`, `call`, `funcs`, `props`,
  `classes`, `watchprop` / `unwatch`, `con <console command>`, `uinfo`
- **export:** `dumpfuncs`, `dumpvtables`, `dumpsdk`, `dumpfieldvt`, `callees`
- **hooks and breakpoints:** `native <name>`, `hook <addr|name> [log|block|ret <v>] [hw] [force]`, `unhook`, `hooks`,
  `bpx` / `bpw` / `bprw` / `bpc` / `bps`, `watchw <addr> <len> secs=<n>` (who writes this field), `agg` (hits grouped by stack)
- **game state:** `scene` (preflight: world, pc, pawn), `comps <actor>`, `pose <actor>`, `tarray|tset|tmap <obj> <Prop.Path>`,
  `layoutcheck <Class>`
- **capture:** `snap` (typed memory fixture -> `snaps\<name>.json`), `sample` (in-core sampler), `memdiff` (which bytes change), `stats`
- **memory scan:** `scan <type> <eq|changed|inc|dec> [v]`, `scan list`, `scan set <v>`, `scan clear` (value hunt on any game)
- **input bot:** `bot key <k> [tap|down|up]`, `bot text <s>`, `bot move <dx> <dy>`, `bot moveto <x> <y>`, `bot click [btn] [x y]`,
  `bot wheel <n>`, `bot home`, `bot release`, `bot focus [on|off]`, `bot status` (any game; injected in-process via window
  messages, key-state polls, raw input and DirectInput, so the real mouse stays free; `focus on` makes a background game
  accept keys; `status` shows which input API the game reads)
- **misc:** `xcall <addr|Name> [rcx rdx r8 r9]`, `mark`, `log [n]`, `plug list|build|reload|unload <name>`

`pawn` and `pc` resolve to the local player's pawn and controller. Commands sent before the engine is up answer
`error: reflection not ready (...)`; the core retries every second.

## MCP tools

`status, find, obj, get, set, call, funcs, props, classes, rd, wr, ptr, sym, addr, names, decomp, native, hook, unhook, hooks, bp,
bpc, bps, hits, watchw, agg, scene, snap, sample, comps, pose, tarray, tset, tmap, layoutcheck, memdiff, stats, batch, mark, watch,
unwatch, plugin, con, log, scan, bot, reload_core, raw`

Every tool takes an optional `game` (port or alias) to target one of several dev games, and `all` runs it on every port.
Results are capped at 12000 characters (`CRAWLER_MCP_CAP`).

## Plugins

`plugins\<name>\*.cpp` -> `bin\plugins\<name>.dll` (`plugins\build_plugin.bat <name>`). The core polls the folder every second. On
save it rebuilds (5-15 s) and swaps the plugin on the game thread: the old `UEPluginShutdown` runs, its hooks are removed, the
fresh copy is loaded and `UEPluginInit` runs. A failed build keeps the old copy running.

```cpp
#include "crawler_api.h"
static const UEApi* api;
UE_EXPORT int UEPluginInit(const UEApi* a) { api = a; return 1; }
UE_EXPORT void UEPluginTick() {
  static uint64_t last; if (api->ticks() - last < 600) return; last = api->ticks();
  char out[512]; uintptr_t pc = api->find("pc");
  if (pc && api->call(pc, "K2_GetActorLocation", "", out, sizeof out)) api->log("pc location %s", out);
}
UE_EXPORT void UEPluginShutdown() {}
```

API (`plugins/crawler_api.h`):

| Member | Meaning |
|---|---|
| `base` | Game exe base address |
| `log(fmt, ...)` | Append to `crawler.log`, prefixed with the plugin name |
| `addr(expr)` | Address of a names-database name, a native, or any Game Crawler expression; 0 = not found |
| `sym(a, buf, n)` | Address -> `exe+0x.. Name+0x..` |
| `hook` / `unhook` | Inline hook (returns 1 on success); hooks are removed automatically on unload |
| `find(obj)` | UObject by `pawn`, `pc`, name, path or `0x` address |
| `get` / `set` / `call` | Property as text / write a property / call a UFunction (queued to the game thread, up to 5 s) |
| `cmd(line, out, n)` | Run any Game Crawler command |
| `ticks()` | Game-thread pump counter |
| `read(a, out, n)` | Fault-safe memory read (1 = ok) |
| `sig(pattern, rip)` | Byte signature in the exe code (`"48 8B ?? ..."`); `rip` >= 0 resolves a RIP-relative operand at that offset; 0 = not found |
| `path(rel, buf, n)` | Absolute path under the Game Crawler folder |
| `describe(a, buf, n)` | `Class /path` of an engine object, empty if `a` is not one |

Exports (all but `UEPluginInit` optional):

| Export | Meaning |
|---|---|
| `int UEPluginInit(const UEApi*)` | Return 1 = ok. `api->version` is 2 |
| `void UEPluginTick()` | Called on the game thread |
| `void UEPluginShutdown()` | Stop and join any threads the plugin started |
| `int UEPluginCommand(const char* line, char* out, int n)` | New commands: return 1 and write the reply to `out` if the line is yours, 0 otherwise. Commands Game Crawler does not know are offered to every plugin, so they work from the CLI, `raw` and MCP |
| `const char* UEPluginHelp()` | One line listing the plugin's commands, shown by `plug list` |
| `uintptr_t UEPluginPreCall()` / `void UEPluginCallFault(uintptr_t)` | Guard around every UFunction call Game Crawler makes: the value returned before the call is handed back if the call faults, to repair game state |

Wrap risky pointer reads in `__try` / `__except` or use `read`.

**MCP tools from a plugin.** `plugins\<name>\mcp.js` is loaded by the MCP server at start. It exports a function that gets the
server helpers and returns tool rows:

```js
module.exports = ({ S, str, int, OBJ, cmd }) => [
  ['hello', 'Say hello from the example plugin.', S({ name: str('who') }), a => cmd('hello', a.name)],
];
```

**Plugins in their own repo.** Clone (or link) a plugin repo into `plugins\<name>`; only `plugins\example` is tracked here.
[Anomaly Scanner](https://github.com/Joaovitor404silva/Anomaly-Scanner) is one: S.T.A.L.K.E.R. 2 quest graph, NPC census and a
scripted player bot on top of Game Crawler.

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
and the PE imports and exports of the exe (read from the running game, or `--exe`). Rerun it after a game update.

## Configuration

| Variable | Default | Used for |
|---|---|---|
| `CRAWLER_HOME` | this folder (set by `launch.ps1`, or `crawler_home.txt` next to the proxy) | Root for `bin\`, `plugins\`, logs, `names\`, `sdk\` |
| `CRAWLER_PORT` | `27060` | TCP port of the dev game (one per game when several run) |
| `CRAWLER_LOGDIR` | `CRAWLER_HOME` | Log folder (`crawler.log`, `crawler_<port>.log`) |
| `CRAWLER_CORE` | `bin\GameCrawlerCore.dll` | Core DLL to load (staged builds) |
| `CRAWLER_NAMES` | `names\names.tsv` | Names database |
| `CRAWLER_NATIVES` | `names\natives_impl.tsv` | Native exec -> impl table |
| `CRAWLER_DECOMP` | `decomp` | Decompile dump folder (MCP `decomp`) |
| `CRAWLER_TIMEOUT`, `CRAWLER_MCP_CAP`, `CRAWLER_ALL_PORTS` | | CLI/MCP timeout, MCP result cap, port list for `game=all` |

## Tests

```powershell
cmd /c tests\run_tests.bat                 # C++ units (engine discovery on fake memory) + core scenario, MCP server and CLI against fake servers
```
No game needed.

## Limits

- 64-bit Windows builds only. Games with anti-cheat (EAC, BattlEye, ...) are not supported; do not try. DRM-wrapped exes (e.g. Denuvo) work when the game runs normally; other packers are untested.
- UE 4.25+ (FField properties). Older UE4 (UProperty) and heavily modified engine forks are not detected yet.
- Console variables (`cvar`) need a per-game `IConsoleManager` address and are off; `con` uses `ExecuteConsoleCommand`.
- Games that already ship a `version.dll` next to the exe need a different proxy name.

## Disclaimer

- **Unofficial.** This project is not affiliated with, endorsed by or supported by Epic Games or any game publisher or
  developer. Unreal Engine is a trademark of Epic Games. All other trademarks belong to their owners.
- **No game content.** This repository contains no game code, assets, decompiled code or data. The names database and the
  decompile are generated locally from your own legally obtained copy and must not be redistributed.
- **Single-player and offline use only.** Do not use it in online or multiplayer sessions, with anti-cheat, or to gain an
  advantage over other players. Doing so may break the game's terms of service and can get your account banned.
- **Use at your own risk.** It injects code into a running process and writes to its memory. It can crash the game, corrupt
  saves or cause data loss. Back up your saves (the dev game uses a separate `Saved_dev` folder, but nothing is guaranteed).
  The software is provided "as is", without warranty of any kind; see `LICENSE`.
- **Your responsibility.** You are responsible for complying with each game's EULA and the laws of your country. Modding and
  reverse engineering for interoperability are allowed in many places but not all.
- **Antivirus.** DLL proxies, injection and inline hooks can trigger antivirus warnings. Build from source and check the code
  yourself if in doubt.
- **AI-assisted.** Parts of this project were written with the help of AI coding tools.

## License

MIT, see `LICENSE`. MinHook (`src/minhook`, submodule) is BSD-2-Clause, copyright Tsuda Kageyu.
