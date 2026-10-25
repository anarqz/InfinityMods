# FH2_ModLoader adapter

Runs a mod written for [FH2_ModLoader](https://github.com/Bullet4fun/FH2_ModLoader) (API 1,
`sdk/fh2_mod.h` of the original) inside the Infinity mod runtime, without changing the mod.

`fh2_modloader_adapter.c` is compiled into the mod's DLL. It exports `InfinityModInitializeV1`;
the mod keeps its own `FH2ModLoad` and `DllMain`.

| FH2_ModLoader | Under Infinity |
|---|---|
| `azimuth-tls.dll` loads `data\bin\mods\*.dll` in DllMain | `infinity_mod.dll` verifies and loads the mod; the adapter starts it from its own thread, outside the loader lock |
| `game_image` | what `wait_for_code(60000)` returns (FAILED after 60 s) |
| `game_exe` | `GetModuleFileNameW` of that image, or an empty string when the image is not a loader module (Azimuth maps the game itself; ABI 1 passes no game folder). Mods that use it (Spotify Radio) need a change |
| `data_dir`, `settings_dir` | `<azimuth>\infinity-mods\<Mod>\` (the folder of the mod's settings file) |
| `mods_dir` | `<azimuth>\infinity-mods\` |
| `game` | always `FH2_GAME_FORZA_HORIZON_2`; a mod whose `FH2ModGames` leaves it out is refused |
| `FH2ModLoad` returns nonzero | READY; zero → FAILED |
| delete the DLL to turn it off | the launcher's Disable, or `[Infinity] Enabled=0` in the mod's settings |

Limits: the mod starts after the game's code is up, not in DllMain before the game starts, so a
mod that must patch before game initialisation needs a check (and possibly source changes). Hooks a
mod installs with its own code bypass the runtime's hook API; such mods need review before they are
listed. The adapter uses only the Win32 functions in the original's `common/kernel32.def`.
