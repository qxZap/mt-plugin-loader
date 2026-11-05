<p align="center">
  <img src=".github/hero.png" alt="Plugin Loader" width="100%">
</p>

# Plugin Loader

Plugin loader for Motor Town.

Only one file can be `version.dll`, so this is it, and every native mod is a
plugin of it.

```
MotorTown\Binaries\Win64\version.dll                         the loader
MotorTown\Binaries\Win64\mt_loader.log / mt_loader.ini       its log, [Mappings] Export=1
MotorTown\Binaries\Win64\plugins\mt_plugin_<Name>.dll        a plugin
MotorTown\Binaries\Win64\plugins\mt_plugin_<Name>.log        its log
```

Build: `build.bat` (MSVC 2022) -> `out\`. Proton: `WINEDLLOVERRIDES="version=n,b" %command%`.

- The ProcessEvent hook is installed as soon as the engine is ready -- **not**
  after the world comes up. ProcessEvent is the only way onto the game thread,
  so anything that needs the game before the first delivery exists (a key
  that opens a panel) waits for a world that may never arrive. The loader's
  log says `ProcessEvent: vtable index N, M slots hooked`; if that line is
  missing, nothing that touches the game thread runs.

## Linux and Steam Deck

Proton loads its own `version.dll` unless told otherwise. Add this to the game's Steam launch
options:

    WINEDLLOVERRIDES="version=n,b" %command%

## Antivirus warnings

Windows Defender and a few other scanners can flag `version.dll`. VirusTotal showed 3 of 60 for
one build (ClamAV `Win.Backdoor.Sagerunex`, Microsoft `Trojan:Win32/Wacatac.C!ml`, Cynet). The
`!ml` means a machine learning guess, not a known virus. The loader has to look suspicious to a
scanner: it sits in the game folder under a Windows dll's name, and it patches the running game.
It makes no network connections, downloads nothing and runs no commands, and the source is here
to read and build yourself.

If yours flags it, get the dll from this repo's releases, compare its SHA-256 with the one
published there, and exclude only the game's `Binaries\Win64` folder. Do not turn Windows
Security off.

## Rules

- Only `plugins\mt_plugin_*.dll` loads, in **alphabetical order** of file name
  (case-insensitive). Anything else in that folder is ignored.
- A plugin exports `int PluginInit(const MtApi*)` (`include/mt_plugin_api.h`),
  returns 0, and does its work on its own thread. A DLL without it is skipped
  without running: the loader checks the export with the file mapped but its
  DllMain not called, because loading and then freeing a DllMain-style DLL
  leaves its threads running in unmapped code -- that crashed the game once.
- **The loader owns everything invasive or shared.** The ProcessEvent hook is
  installed once, by the loader: a second hook on the same vtable slots finds
  them already swapped and breaks. Plugins get `run_on_game_thread` and
  `process_event` instead. Engine discovery (name pool, object table,
  UStruct/FField layout, FindObject, field offsets) also happens once, there.
- The api is a versioned struct of function pointers; fields are only appended,
  so a plugin built against v1 keeps working on later loaders.

## The api (v6)

`include/mt_plugin_api.h` is the whole contract. What a plugin gets:

| group | what |
|---|---|
| logging | `log(self, fmt, ...)` -- the plugin's own log file, one line per call |
| engine | `wait_engine`, `layout`, `read`, `write`, `name`, `fname`, `name_index`, `num_objects`, `object`, `find_object`, `is_a`, `field`, `field_offset` |
| the game thread | `run_on_game_thread(fn, ctx)`, `process_event(obj, func, parms)`, `game_thread_ready` |
| **watching** | `watch_functions(names, count, fn, ctx)` -- called back on the game thread when one of those UFunctions fires; `fn` may read **or edit** the parameter block in place, and returning non-zero blocks the original call |
| **patching** | `patch_bytes(addr, bytes, len)` -- overwrite game code (writable for the write, protection restored, instruction cache flushed) |
| **ini files** | `ini_set(self, file, section, key, value, force)` -- declare a value in one of the loader's ini files; `ini_get(file, section, key, buf, cap)` -- read the merged value back |

Two things a plugin author needs to know, both learned the hard way:

- **The ProcessEvent hook does not see RPCs.** UE dispatches an RPC straight to
  the exec thunk. Blueprint events are visible (`ReceiveSetVehicle` fires on
  every vehicle swap); `ServerChangeVehicle` and `ServerBoughtVehicle` never
  fire, on either side. Anything that has to see an RPC needs `watch_functions`
  on the exec side, a native hook, or a code patch.
- `watch_functions` resolves each name to an FName index once and compares
  indices per call, so watching is cheap. Registering the same callback and ctx
  again is ignored; another callback on the same function is a separate watcher,
  so two plugins can watch one function. A name whose class is not loaded yet is
  simply not registered (retry). Watchers on one function run in registration
  order, and one that blocks the call also stops the ones after it.

## Ini files: the loader owns them

Plugins do not write `.ini` files themselves. They declare values and the
loader merges them, so two plugins that write the same file cannot overwrite
each other -- which is the whole point: `WritePrivateProfileString` rewrites
the file it is given, so two plugins doing that to the same file race, and the
loser takes the winner's file with it.

```c
// a default: the file's own value wins, so a hand-edited file is kept
iniSet(L"custom_wheel_scale.ini", L"WheelScale", L"Min", L"0.1");
// live state: written through at once, always wins
iniSet(L"moretuning.ini", section, key, value, true);
// what the merged view says
std::wstring v = iniGet(L"custom_wheel_scale.ini", L"WheelScale", L"Min");
```

`mt::iniNum` / `mt::iniInt` in `include/mt_plugin.hpp` do the declare-then-read
dance in one call, which is what most settings want.

The rules, in order of precedence:

- **a value already in the file wins** over a plugin's default. These files
  are edited by hand and what the user typed is kept;
- **two plugins declaring the same key**: the one that *loaded* first wins --
  not the one that declared first, since plugins declare on their own threads
  and that order is not fixed. The loser is refused and the conflict is
  logged, with both values;
- **a forced value** is written through at once and always wins. That is for
  live state, which must not wait for anything;
- only the keys a plugin declared are ever written, one key at a time, so the
  rest of the file -- other plugins' keys, the game's own keys, the comments
  in it -- is left exactly as it was.

A file name is a path relative to the game folder (`Binaries\Win64`), where
these files have always lived. `..` and absolute paths are refused.

`tools\ini_test.cpp` checks all of this without the game in the loop;
`build.bat` runs it.
