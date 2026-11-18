# AGENTS.md -- Plugin Loader

`version.dll` for Motor Town: it loads `plugins\mt_plugin_*.dll` and gives them one shared
way into the game. README.md holds the rules and the api.

## Build and test

`build.bat` (MSVC 2022 x64) writes `out\version.dll` and runs `tools\ini_test.cpp`, the check
for the ini merge, the one part of the loader with no game in the loop.

## Rules

- `include/mt_plugin_api.h` is the contract. Fields are only appended and `MT_API_VERSION`
  goes up, so older plugins keep working.
- The loader owns the ProcessEvent hook and engine discovery, once. Plugins never do either.
- Layouts are detected, not assumed: an engine or game update should change a log line,
  not crash.
- `[Mappings] Export=1` in `mt_loader.ini` writes `Mappings.usmap` about 10 seconds after
  the game's systems load, so a crash at startup means no dump.
- Every plugin repo carries a copy of `include/`. Changing the api means updating those.

## Releases

Released dlls are built by GitHub, not on a dev machine, so anyone (Nexus's reviewers too) can
match a download to the source. Branch `release/<version>` off main and push it:
`.github/workflows/release.yml` runs `build.bat` (ini test included) and publishes the GitHub
release `v<version>` with the drag-and-drop zip, the bare `version.dll` and `SHA256SUMS.txt`.
Pushing the branch again rebuilds that release. Bump `src/version.rc` for a new version.
