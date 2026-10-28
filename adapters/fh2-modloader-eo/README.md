# FH2_ModLoader adapter for ExtraOptions

Runs a mod written for [FH2_ModLoader](https://github.com/Bullet4fun/FH2_ModLoader) (API 1,
`sdk/fh2_mod.h` of the original) as a plugin of ExtraOptions' mod loader (ExtraOptions 0.3.9 on
Azimuth 1.0.3, built into Infinity as ExtraOptions.Infinity), without changing the mod and without the
Infinity mod runtime. `fh2_modloader_eo_adapter.c` is compiled into the mod's DLL. It exports
`FH2PluginInitialize` (ExtraOptions' plugin ABI 1, `fh2_plugin.h` and `tools/PLUGIN_API.md`) and calls
the mod's own `FH2ModLoad`; the mod keeps its `DllMain`.

The DLL is `<Azimuth>\mods\<id>\plugins\<dll>`; ExtraOptions loads it when the folder is enabled in
`mods\loadorder.txt`. The folder's `mod.json` sets `database`, `media` and `strings` to `null`: a
plugin-only mod is active as soon as ExtraOptions reads its manifest.

| FH2_ModLoader | Under ExtraOptions |
|---|---|
| `azimuth-tls.dll` loads `data\bin\mods\*.dll` and calls `FH2ModLoad` in DllMain | ExtraOptions loads `plugins\*.dll` on its worker after the game's first file lookup and calls `FH2PluginInitialize` after DllMain returned, outside the loader lock; the adapter calls `FH2ModLoad` from there |
| `game_image` | the context's `game_image` (the mapped, relocated FH2 image) |
| `game_exe` | `GetModuleFileNameW` of that image, or an empty string (Azimuth maps the game itself; the context passes no game folder). Mods that use it (Spotify Radio) need a change |
| `data_dir`, `settings_dir` | `<Azimuth>\mods\<id>\` (the parent of `plugins\`) |
| `mods_dir` | `<Azimuth>\mods\` |
| `game` | always `FH2_GAME_FORZA_HORIZON_2`; a mod whose `FH2ModGames` leaves it out is refused |
| `FH2ModLoad` returns nonzero | `TRUE` (ExtraOptions logs `initialized`); zero → `FALSE` (logged, DLL kept, no callbacks) |
| delete the DLL to turn it off | the launcher's Disable (`# <id>` in `loadorder.txt`), or `[Infinity] Enabled=0` in `<Azimuth>\mods\<id>\<DLL name>.ini` |

`FH2PluginInitialize` returns `FALSE` when the context is not ABI 1 for Azimuth, the DLL is not in a
mod's `plugins\`, or the mod is not made for Forza Horizon 2. Log lines go to `FH2_ExtraOptions.log`.

Limits: the mod starts after the game's first file lookup, not in DllMain before the game starts, so
a mod that must patch before game initialisation needs a check (and possibly source changes). Hooks a
mod installs with its own code are its own; such mods need review before they are listed. The adapter
uses only the Win32 functions in the original's `common/kernel32.def` (Zig toolchain, freestanding).
