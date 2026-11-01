# Writing a Game Crawler plugin

A plugin is a C++ DLL that Game Crawler loads inside the game. It can hook functions, read and write memory, use the
engine tools (find objects, get/set properties, call functions), add its own commands, add MCP tools, and run code every
game tick. Plugins hot reload: save a source file and Game Crawler rebuilds and swaps the plugin while the game runs.

Example: [`plugins/example`](plugins/example). A full game plugin:
[Anomaly Scanner](https://github.com/Joaovitor404silva/Anomaly-Scanner) (S.T.A.L.K.E.R. 2).

## 1. Create the folder

```
plugins\
  mygame\
    mygame.cpp      every top-level .cpp is compiled into bin\plugins\mygame.dll
    mcp.js          optional: MCP tools
    test\build.bat  optional: unit tests (run by CI)
```

The folder name is the plugin name. Use lower case letters, digits and `-`.

## 2. Minimal plugin

```cpp
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "crawler_api.h"

static const UEApi* api;

UE_EXPORT int UEPluginInit(const UEApi* a) {
  api = a;
  if (a->version < 2) return 0;
  api->log("mygame loaded");
  return 1;
}

UE_EXPORT void UEPluginTick() {
  static uint64_t last; if (api->ticks() - last < 600) return; last = api->ticks();
  char out[512]; uintptr_t pawn = api->find("pawn");
  if (pawn && api->call(pawn, "K2_GetActorLocation", "", out, sizeof out) == 1) api->log("pawn %s", out);
}

UE_EXPORT void UEPluginShutdown() {}
```

## 3. Build and load

Start the dev game (see the README). Game Crawler watches `plugins\` every second: a new or changed plugin is built
(5-15 s) and loaded. By hand:

```
node crawler.js "plug build mygame"     build + load (build log: bin\plugins\mygame.build.log)
node crawler.js "plug list"             state, hooks and commands of every plugin
node crawler.js "plug log mygame"       last build log
node crawler.js "plug unload mygame"
```

A failed build keeps the old copy running. Messages from `api->log` go to `crawler.log` with the plugin name in front.

## 4. Add a command

Commands Game Crawler does not know are offered to every loaded plugin. Return 1 if the line is yours.

```cpp
UE_EXPORT const char* UEPluginHelp() { return "where"; }

UE_EXPORT int UEPluginCommand(const char* line, char* out, int n) {
  if (strcmp(line, "where") != 0) return 0;
  uintptr_t pawn = api->find("pawn");
  if (!pawn || api->call(pawn, "K2_GetActorLocation", "", out, n) != 1) snprintf(out, n, "error: no pawn");
  return 1;
}
```

`node crawler.js where` and the MCP `raw` tool now run it. `UEPluginHelp` is the line shown by `plug list`. Commands run on the
Game Crawler server thread, not the game thread; `get`, `set` and `call` queue themselves to the game thread.

## 5. Add MCP tools

`plugins\mygame\mcp.js` is loaded when the MCP server starts (restart your MCP client after adding it):

```js
'use strict';
module.exports = ({ S, str, int, OBJ, cmd }) => [
  ['where', 'Location of the player pawn.', S({}), () => cmd('where')],
  ['teleport', 'Move an actor.', S({ obj: OBJ, xyz: str('x,y,z') }, ['obj', 'xyz']), a => cmd('call', a.obj, 'K2_SetActorLocation', a.xyz, 'false', 'false')],
];
```

Each row is `[name, description, schema, handler]`. `S(props, required)` builds the schema, `cmd(...)` sends a command line to the
game and returns its reply. Every tool also gets the shared `game`, `json` and `out_file` parameters. A name that a core tool
already uses is ignored. Check the file with `node tests\check_plugin_mcp.js plugins\mygame\mcp.js`.

## 6. Keep it to one game

A game plugin must stay idle in other games. Check the exe in `UEPluginInit`, return 1 and do nothing:

```cpp
char exe[MAX_PATH] = {}; GetModuleFileNameA(nullptr, exe, MAX_PATH);
g_on = strstr(_strlwr(exe), "mygame") != nullptr;
```

## 7. Tests

Put pure logic (parsers, math, layouts read through a read function) in headers and test them from `test\build.bat`, which must
exit non-zero on failure. It runs from `plugins\<name>\test`, so `call "%~dp0..\..\..\src\vcvars.bat"` sets up MSVC.
CI runs it on every pull request.

## 8. Publish it in its own repo

Keep your plugin in its own repo with the plugin files at the root; users clone it into `plugins\<name>`. Add
`.github/workflows/ci.yml` to it so every pull request is built and tested against Game Crawler:

```yaml
name: CI
on: [pull_request, push, workflow_dispatch]
jobs:
  plugin:
    uses: Joaovitor404silva/Game-Crawler/.github/workflows/plugin.yml@main
    with:
      name: mygame
```

It builds the plugin, runs `test\build.bat`, checks `mcp.js` and runs the Game Crawler tests with your plugin in place.

## API reference

`plugins/crawler_api.h`, `api->version` 2:

| Member | Meaning |
|---|---|
| `base` | Game exe base address |
| `log(fmt, ...)` | Append to `crawler.log`, prefixed with the plugin name |
| `addr(expr)` | Address of a names-database name, a native, or any Game Crawler expression; 0 = not found |
| `sym(a, buf, n)` | Address -> `exe+0x.. Name+0x..` |
| `hook(target, detour, &orig)` / `unhook(target)` | Inline hook (1 = ok); removed automatically on unload |
| `find(obj)` | Engine object by `pawn`, `pc`, name, path or `0x` address |
| `get` / `set` / `call` | Property as text / write a property / call a UFunction (queued to the game thread, up to 5 s) |
| `cmd(line, out, n)` | Run any Game Crawler command |
| `ticks()` | Game-thread pump counter |
| `read(a, out, n)` | Fault-safe memory read (1 = ok) |
| `sig(pattern, rip)` | Byte signature in the exe code (`"48 8B ?? ..."`); `rip` >= 0 resolves a RIP-relative operand at that offset; 0 = not found |
| `path(rel, buf, n)` | Absolute path under the Game Crawler folder |
| `describe(a, buf, n)` | `Class /path` of an engine object, empty if `a` is not one |

Exports (only `UEPluginInit` is required):

| Export | Meaning |
|---|---|
| `int UEPluginInit(const UEApi*)` | Return 1 = ok |
| `void UEPluginTick()` | Called on the game thread |
| `void UEPluginShutdown()` | Undo your work; stop and join any threads you started, or the DLL cannot be freed |
| `int UEPluginCommand(const char* line, char* out, int n)` | Your commands; 1 = handled |
| `const char* UEPluginHelp()` | One line listing your commands |
| `uintptr_t UEPluginPreCall()` / `void UEPluginCallFault(uintptr_t)` | Guard around every UFunction call Game Crawler makes: the value returned before the call comes back if the call faults, to repair game state |

## Rules

- Never block the game thread in `UEPluginTick`; keep it short.
- Wrap raw pointer reads in `__try` / `__except`, or use `api->read`.
- Exceptions thrown by a plugin are caught and counted (`tickExc` in `plug list`), but a corrupted game state is not; test in a
  dev copy of the game.
- No anti-cheat games, single-player only (see the Disclaimer in the README).
