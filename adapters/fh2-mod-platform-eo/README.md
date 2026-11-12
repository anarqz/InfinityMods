# FH2 Mod Platform adapter for ExtraOptions

Runs a mod written for [FH2 Mod Platform](https://github.com/Trinculo54/fh2-mod-platform) (API 9, platform 2.8.0,
`include/fh2mod.h`) as a plugin of ExtraOptions' mod loader (ExtraOptions 0.6.0 on Azimuth 1.0.4, built into Infinity
as ExtraOptions.Infinity, OD-126/OD-127; the plugin ABI, its 64-byte context and the database frame are unchanged
since 0.3.2), without the platform (`DWrite.dll`, `fh2-mods\`) and without the Infinity mod runtime.

The adapter is its own DLL, `<Azimuth>\mods\Forza Horizon 2\<id>\plugins\<id>.Infinity.dll` (ExtraOptions loads
it when the folder is enabled in `mods\Forza Horizon 2\loadorder.txt`). The mod is the **author's own release DLL,
unchanged**: the manifest downloads the author's release ZIP, checks it by SHA-256 and installs the DLL at
`<id>\<adapterConfig.modDll>` (for example `fh2-mods\FnFNitro.dll`), outside `plugins\` so ExtraOptions never loads it
by itself. Nothing of the mod is compiled or shipped here. That matters: these mods embed game-derived files (HUD
XAML, camera presets, textures) in their DLL, which must never be in an Infinity package (the monorepo's AGENTS.md,
rule 3), and an upstream update needs only a new pin (`tools/infmods.py sync`), not a port.

`fh2_mod_platform_eo_adapter.cpp` and MinHook (the copy in the mod's `platform/third_party/minhook`, at the commit the
release was built from) are compiled with MSVC (`toolchain: msvc`) against the mod's own `platform/include/fh2mod.h`.
`tools/infmods.py` writes `fh2_mod_platform_eo_config.h` (`FH2MP_EO_MOD_DLL`) from the recipe. The DLL exports
`FH2PluginInitialize`, `FH2PluginMediaDirectory` and `FH2PluginDatabaseUpdate` (ExtraOptions' plugin ABI 1,
`fh2/tools/PLUGIN_API.md`): it loads the mod's DLL with its full path and calls its `fh2mod_init` with an
`FH2ModApi` table it implements itself.

| FH2 Mod Platform | Under ExtraOptions |
|---|---|
| `hook_function`, `hook_call` | one hook broker per process, shared by every adapter DLL (the first to start publishes it in a per-process named mapping; only its MinHook patches), chained like the platform's `hooks.cpp`: a patched place's entry stub jumps to the newest enabled hook's detour, each hook's `*original` is a stub of its own pointing at the next older one, the oldest at the real code (MinHook's trampoline, or the `E8` call's old target). So NitroSystem and Underglow can both hook `ForzaCar::Update`. Created disabled |
| `enable_hooks`, `disable_hooks` | one of this mod's hooks, or all of them with `nullptr`; a place is patched while any hook on it is enabled and restored when none is; 1 when all succeeded |
| `hook_pointer`, `patch_bytes`, `hook_instruction` | not supported: logged, `nullptr` (a mod that needs them is not adapted yet) |
| `wait_for_code`, `wait_for_game`, `game_base` | the context's `game_image` (the mapped FH2 image; ExtraOptions calls the initializer after the game's first file lookup) |
| `azimuth_dir` | the mod's own folder (`<Azimuth>\mods\Forza Horizon 2\<id>`, the parent of `plugins\`): its INI (`fh2-mods\<mod>.ini`) and its per-car files (`data\titles\1A96545B\<mod>\player-1\`) stay there. It never sees the real saves, so every player profile shares `player-1`, and uninstalling the mod deletes those files with its folder. The adapter refuses a mod folder longer than 160 characters (the mods build `MAX_PATH` paths below it) |
| `game_dir` | empty (a mod's cleanup of old `mediaoverride` files does nothing) |
| `set_file`, `clear_file` | while `fh2mod_init` runs, written at once to `<id>\plugins\generated\<media path>` (the result is the real write's; the previous start's tree is removed first, never following a link) and returned by `FH2PluginMediaDirectory`: ExtraOptions reads it as the mod's private media, a zip's name standing in for the zip as in the platform. Paths with `..`, a device name or a part ending in `.` or space are refused. Later calls are refused (logged) |
| `add_sql` | queued (64 calls, 1 MiB each, several statements per call) and run, statement by statement, by `FH2PluginDatabaseUpdate` on ExtraOptions' merged in-memory database inside one savepoint; the folder ships an empty `database.json` so ExtraOptions builds that merge |
| `register_mod`, `log` | one mod per DLL; ExtraOptions' log (`FH2_ExtraOptions.log`) |
| `add_setting`, sliders, `set_setting` | accepted, never shown: the launcher's form owns the INI, so `on_change` never runs; `get_setting` is -1 |
| `publish_interface`, `get_interface`, `wait_for_mods` | no other mods' interfaces: `get_interface` is `nullptr` (FH2 ANNA menus and similar links are off), `wait_for_mods` returns at once |
| string tables, notices, Mods pages, title entries, hotkeys, commands, session logs, save snapshots, dump ranges | not provided: each reports "not added" (0 / `nullptr`) |
| `playing_online` | 0 (Infinity decides where a mod may run: a mod whose impact is not cosmetic is Noob only) |

`FH2PluginInitialize` returns FALSE (ExtraOptions logs it and keeps the DLL loaded without callbacks) when the context
is not ABI 1 for Azimuth, the DLL is not in `plugins\`, MinHook cannot start, the mod's DLL is missing or has no
`fh2mod_init`, or `fh2mod_init` fails (its hooks are then switched off and its files removed). Upstream's own race
stays as it is under the platform: Underglow checks `ForzaCar::Update`'s stock bytes before hooking it, so when
NitroSystem has already patched it, Underglow logs that it does not know the build and stays off for that start. A mod built against another platform API than 9 does not compile here
(`static_assert`): `tools/infmods.py sync` reports it, and the table must be updated for the new header first.

ExtraOptions activates a mod's private media only when its ordinary `media` folder exists at startup, so every package
ships `media/README.txt` (this folder's `media-README.txt`).
