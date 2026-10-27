# FH2 Mod Platform adapter for ExtraOptions

Runs a mod written for [FH2 Mod Platform](https://github.com/Trinculo54/fh2-mod-platform) (API 8,
`include/fh2mod.h`) as a plugin of ExtraOptions' mod loader (ExtraOptions 0.3.2, built into Infinity as
ExtraOptions.Infinity, OD-126), without changing the mod and without the Infinity mod runtime.
`fh2_mod_platform_eo_adapter.cpp` and MinHook (the copy in the mod's `platform/third_party/minhook`) are
compiled into the mod's DLL. It exports `FH2PluginInitialize` and `FH2PluginDatabaseUpdate` (ExtraOptions'
plugin ABI 1, `tools/PLUGIN_API.md`) and calls the mod's own `fh2mod_init` with an `FH2ModApi` table it
implements itself.

The DLL is `<Azimuth>\mods\<id>\plugins\<dll>`; ExtraOptions loads it when the folder is enabled in
`mods\loadorder.txt`.

| FH2 Mod Platform | Under ExtraOptions |
|---|---|
| `hook_function` | MinHook (`MH_CreateHook`), created disabled |
| `hook_call` | the `E8` call's rel32 redirected through a jump stub allocated within ±2 GB; `*original` = the old target |
| `enable_hooks`, `disable_hooks` | one hook, or every hook with `nullptr`; 1 when all succeeded |
| `hook_pointer`, `patch_bytes`, `hook_instruction` | not supported: logged, `nullptr` (a mod that needs them is not adapted yet) |
| `wait_for_code`, `wait_for_game`, `game_base` | the context's `game_image` (the mapped FH2 image; ExtraOptions calls the initializer after the game's first file lookup) |
| `azimuth_dir` | the mod's own folder (`<Azimuth>\mods\<id>`, the parent of `plugins\`): its INI and sounds stay there |
| `game_dir` | empty |
| `register_mod`, `log` | one mod per DLL; ExtraOptions' log (`FH2_ExtraOptions.log`) |
| `add_sql` | queued (16 statements, 4 KiB each) and run by `FH2PluginDatabaseUpdate` on ExtraOptions' merged in-memory database inside a savepoint; the folder ships an empty `database.json` so ExtraOptions builds that merge |
| `add_setting`, sliders, `get/set_setting` | accepted, never shown: the launcher's form owns settings, so `on_change` never runs and the mod never writes its INI |
| string tables, notices, `set_file`, Mods pages, interfaces | no-ops returning failure or nothing, logged |

`FH2PluginInitialize` returns FALSE (ExtraOptions logs it and keeps the DLL loaded without callbacks) when
the context is not ABI 1 for Azimuth, the DLL is not in `plugins\`, MinHook cannot start or `fh2mod_init`
fails. Builds with MSVC (`toolchain: msvc`) when the mod's source needs it (`__try`).
