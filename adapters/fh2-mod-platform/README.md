# FH2 Mod Platform adapter

Runs a mod written for [FH2 Mod Platform](https://github.com/Trinculo54/fh2-mod-platform) (API 8,
`include/fh2mod.h`) inside the Infinity mod runtime, without changing the mod.
`fh2_mod_platform_adapter.cpp` is compiled into the mod's DLL with the recipe's generated
`adapter_config.h`; it exports `InfinityModInitializeV1` and `InfinityModRegisterDatabaseV1` and
calls the mod's own `fh2mod_init` with an `FH2ModApi` table backed by the runtime.

| FH2 Mod Platform | Under Infinity |
|---|---|
| `hook_function`, `hook_call`, `hook_pointer`, `patch_bytes`, `hook_instruction`, `enable_hooks`, `disable_hooks` | the runtime's own (same arguments; `FH2Context` = `InfinityContext`) |
| `wait_for_code`, `wait_for_game` | the runtime's, at most 60 s; `game_base` = `wait_for_code(0)` |
| `azimuth_dir` | the directory that makes `<azimuth_dir>\<settingsRelative>` the launcher's settings file: the mod's paths land in `infinity-mods\<Mod>\` |
| `game_dir` | empty (ABI 1 passes no game folder) |
| `register_mod`, `log` | the runtime's mod handle and log |
| `add_setting`, sliders, `get/set_setting` | accepted, never shown: the launcher's form owns settings, so `on_change` never runs and the mod never writes its INI |
| `add_sql` in the game | ignored; the recipe's `startupSql` is registered by `InfinityModRegisterDatabaseV1` in Infinity's preparation process (never `fh2mod_init` there) |
| string tables, notices, `set_file`, Mods pages, interfaces | no-ops returning failure or nothing, logged |
| READY | when every `requiredHooks` name is made and enabled; FAILED if one cannot be, if `fh2mod_init` fails, or after `readyTimeoutMs` |

Recipe (`mods/<id>/mod.json`, `adapterConfig`): `settingsRelative`, `platformVersion`,
`readyTimeoutMs`, `requiredHooks` (the hook names the mod passes), `startupSql`
(`{section, key, default, sql}`: the statement is queued when the INI value is non-zero; the same
condition as the mod's own `add_sql`). The manifest's `config.path` must be
`infinity-mods/<folder>/<settingsRelative>` (`infmods.py selftest` checks it).

Builds with MSVC (`toolchain: msvc`) when the mod's source needs it (`__try`).
