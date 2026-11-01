# Official plugins

Plugins reviewed and approved by the Game Crawler maintainer. Each one has its own repo; clone it into `plugins\<name>`.

| Plugin | Game | Engine | What it adds | Repo |
|---|---|---|---|---|
| `anomaly-scanner` | S.T.A.L.K.E.R. 2: Heart of Chornobyl | Unreal 5.5 | Quest objectives and goals, NPC list, UID lookup, nbot input bot, `ProcessEvent` crash guard; MCP tools `quest`, `npcs`, `uid` | [Joaovitor404silva/Anomaly-Scanner](https://github.com/Joaovitor404silva/Anomaly-Scanner) |

Any plugin works without being on this list. Approval only means it was reviewed and is listed here.

## Get your plugin approved

1. Write it with [PLUGINS.md](PLUGINS.md) and publish it in its own **public** repo with the plugin files at the root.
2. Meet every requirement below.
3. Open a pull request to Game Crawler that adds one row to the table above (plugin folder name, game, engine, what it
   adds, repo link). Fill in the pull request template, including the game and version you tested on.
4. The maintainer reviews the plugin code and the CI run. Approval is the code-owner review on that pull request; once it
   is merged, the plugin is official.

### Requirements

- An open source license file (MIT, Apache-2.0, BSD, GPL, ...).
- `.github/workflows/ci.yml` that uses `Joaovitor404silva/Game-Crawler/.github/workflows/plugin.yml@main`, green on the
  latest commit of the default branch.
- Builds with `plugins\build_plugin.bat <name>` and exports `UEPluginInit`. If it has `mcp.js`, it passes
  `node tests/check_plugin_mcp.js`.
- Stays idle in every other game: checks the exe name or the engine in `UEPluginInit` (see "Keep it to one game" in
  PLUGINS.md).
- Unit tests in `test\build.bat` for its own logic.
- A README with what it does, the game version it was tested on, and how to install it.
- Single-player or private co-op only. No anti-cheat games, no cheats for online play, no bypassing DRM.
- No game files, no decompiled game code, no keys or personal data in the repo.

### Staying official

A plugin is removed from the list if its CI stays red, its repo goes private or away, or it breaks the rules above. An
update to a listed plugin needs no new pull request; a change of repo, folder name or game does.
