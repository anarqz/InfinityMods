# Infinity Mods

Forza Horizon 2 mods adapted to run as plugins of ExtraOptions' native mod loader (ExtraOptions 0.3.9
on Azimuth 1.0.3, built into [Infinity Horizon](https://github.com/anarqz/InfinityHorizon) as
ExtraOptions.Infinity), with credit to the people who made them. Each mod is the original author's
source, pinned as a Git submodule and built unchanged, plus a small adapter that connects the mod's
original loader API to ExtraOptions' plugin ABI (`FH2PluginInitialize`). Each package is one
ExtraOptions mod folder, `<Azimuth>\mods\<id>\`: its `mod.json`, the DLL in `plugins\`, its licences.
The Infinity mod runtime (`infinity_mod.dll`) is retired: no mod here needs it.

| Mod | Original | Adapter | Licence |
|---|---|---|---|
| [Nitro System](mods/nitrosystem) | [qiangqiang101/NitroSystem](https://github.com/qiangqiang101/NitroSystem) | [FH2 Mod Platform for ExtraOptions](adapters/fh2-mod-platform-eo) | MIT |

Install them from the Infinity launcher's Mods page. They do not need the loaders they were
written for (FH2_ModLoader, FH2 Mod Platform), nor a separate ExtraOptions; do not install both.

## Layout

```text
upstream/<name>/          original repositories, Git submodules pinned to an exact commit (never edited here)
adapters/<loader>-eo/     one ExtraOptions plugin adapter per original loader API (MIT)
mods/<id>/mod.json        build recipe: which upstream sources, which adapter, DLL name, version
mods/<id>/manifest.json   Infinity manifest (runtime standalone, files under mods/<id>/); the release fills hashes and the commit
mods/<id>/extraoptions/   the package's own ExtraOptions mod.json (format_version 1; same id and version), database.json if any
mods/<id>/LICENSE, NOTICE.md   the original licence and the attribution shipped in the package
tools/infmods.py          build, verify, release (Python 3.10+, standard library only)
```

## Build

Python 3.10+, and per recipe `toolchain`: [Zig 0.15.2](https://ziglang.org/download/) (the official
archive, any OS) or MSVC (Windows, Visual Studio C++ x64 tools).

```sh
git submodule update --init --recursive
python3 -I tools/infmods.py build  nitrosystem                      # Windows (MSVC)
python3 -I tools/infmods.py verify nitrosystem                      # Windows (MSVC)
```

The game files a mod needs (HUD, particles, shaders, sounds) are never in this repository or in its
packages: the manifest downloads them from the original author's release and checks each by SHA-256.

## Release

One GitHub release per drop, one ZIP per mod (`<id>-<version>.zip`, deterministic).

```sh
python3 -I tools/infmods.py release --zig /path/to/zig --tag <tag> --catalog <current catalog.json>
```

writes `dist/assets/*.zip`, `dist/manifests/*.json`, `dist/catalog.json` and `dist/RELEASE.txt`.
No entry requires `infinity-mod-runtime`; the release drops that retired entry from the catalog it writes.
A mod whose ZIP is byte-identical to the one its catalog entry already points to keeps that entry
unchanged (old URL), so players get no update; its ZIP may still be attached to the new release.
A mod whose ZIP changed needs a new `version` in `mod.json` and `manifest.json`, and its entry
points at the new release. The tool uploads nothing.

## Adding a mod

1. `git submodule add <original repository> upstream/<name>` and check out the exact release commit.
2. `mods/<id>/`: `mod.json`, `manifest.json`, the original `LICENSE`, a `NOTICE.md` naming the authors.
3. If the original loader has no ExtraOptions adapter yet, write one under `adapters/<loader>-eo/`
   (exports `FH2PluginInitialize`, ExtraOptions' `fh2_plugin.h`, ABI 1). Mods that need source
   changes (their own hook code, for example) use a fork as the submodule, with the changes as
   reviewed commits on top of the original.
4. `verify`, then a native check through the Infinity launcher before the catalog lists it.
