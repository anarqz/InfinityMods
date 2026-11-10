# Infinity Mods

Forza Horizon 2 mods adapted to run as plugins of ExtraOptions' native mod loader (ExtraOptions 0.6.0
on Azimuth 1.0.4, built into [Infinity Horizon](https://github.com/anarqz/InfinityHorizon) as
ExtraOptions.Infinity), with credit to the people who made them. Each mod is the original author's
source, pinned as a Git submodule and built unchanged, plus, when it was written for another loader, a
small adapter that connects that loader's API to ExtraOptions' plugin ABI (`FH2PluginInitialize`).
Each package is one ExtraOptions mod folder in its game's mods folder (ExtraOptions 0.6.0 keeps one per
game): `<Azimuth>\mods\Forza Horizon 2\<id>\` (or `…\Forza Motorsport 5\<id>\` for a manifest whose
`games` is `["fm5"]`): its `mod.json`, the DLL in `plugins\`, its licences. The Infinity mod runtime
(`infinity_mod.dll`) is retired: no mod here needs it.

| Mod | Original | Adapter | Licence | Modes |
|---|---|---|---|---|
| [Nitro System](mods/nitrosystem) | [qiangqiang101/NitroSystem](https://github.com/qiangqiang101/NitroSystem) | [FH2 Mod Platform for ExtraOptions](adapters/fh2-mod-platform-eo) | MIT | Noob |
| [Garage Revving](mods/extraoptions-garagerevving) | [Extra Options](https://github.com/RokkuDayo/ForzaHorizon2_ExtraOptions) `fh2/plugins/garage-engine-rev` | none (an ExtraOptions plugin) | GPL-3.0 | both |
| [Weather Control](mods/extraoptions-weathercontrol) | [Extra Options](https://github.com/RokkuDayo/ForzaHorizon2_ExtraOptions) `fh2/plugins/weather-control` | none (an ExtraOptions plugin) | GPL-3.0 | Noob |
| [Spotify Radio](mods/spotifyradio) | [Bullet4fun/FH_ModsCollection](https://github.com/Bullet4fun/FH_ModsCollection) `spotify-radio` (an Azimuth Mod Loader mod) | none: a fork whose `INFINITY_EO` build is an ExtraOptions plugin (`upstream/fh-mods-collection/infinity/README.md`) | GPL-3.0-or-later | both |
| [Camera Mods](mods/camera-mods) | [Deuquan/FH2-CameraMods](https://github.com/Deuquan/FH2-CameraMods) `plugin/` | none (an ExtraOptions plugin) | GPL-3.0 | Noob |

Install them from the Infinity launcher's Mods page. They do not need the loaders they were
written for (FH2_ModLoader, FH2 Mod Platform), nor a separate ExtraOptions; do not install both.
Upstream's own Model Fixes and FM5 Cut Rims Restored mods are game files and are never packaged here.

## Layout

```text
upstream/<name>/          original repositories, Git submodules pinned to an exact commit (never edited here; a fork when a mod needs reviewed changes)
adapters/<loader>-eo/     one ExtraOptions plugin adapter per original loader API (MIT)
mods/<id>/mod.json        build recipe: which upstream sources, which adapter (null: an ExtraOptions plugin), DLL name, version
mods/<id>/manifest.json   Infinity manifest (runtime standalone, files under mods/<game folder>/<id>/); the release fills hashes and the commit
mods/<id>/extraoptions/   the package's own ExtraOptions mod.json (format_version 1; same id and version), database.json if any
mods/<id>/LICENSE, NOTICE.md   the original licence and the attribution shipped in the package
catalog.json              the official mod catalog the Infinity launcher reads from main
schemas/                  JSON Schema of catalog.json
tools/infmods.py          build, verify, release, check (Python 3.10+, standard library only)
```

## Catalog

The Infinity launcher reads `catalog.json` straight from this repository's `main` branch
(raw.githubusercontent.com), keeps a cached copy and falls back to a copy built into the launcher when
offline. A change merged to `main` reaches every player at once, so only the owner (@anarqz,
`.github/CODEOWNERS`) approves changes to it or to `schemas/`.

```sh
python3 -I tools/infmods.py check      # schema, no duplicate ids, every file in its game's mods folder
```

Each mod is offered for the games its manifest declares (`games`, FH2 when absent) and is either for both
modes (its `multiplayer.impact` is `cosmetic`: nothing other players see, hear or race against) or for Noob
only (any other impact). There are no parity exemptions any more (`parity-non-required.json` is retired):
Noob takes any mod. The launcher still reads entries written for the old flat layout (`mods/<id>/…`) as their
game's folder, so entries published before ExtraOptions 0.6.0 keep working.
## Build

Python 3.10+, and per recipe `toolchain`: [Zig 0.15.2](https://ziglang.org/download/) (the official
archive, any OS) or MSVC (Windows, Visual Studio C++ x64 tools).

```sh
git submodule update --init --recursive
python3 -I tools/infmods.py build  nitrosystem                      # Windows (MSVC)
python3 -I tools/infmods.py verify nitrosystem                      # Windows (MSVC)
python3 -I tools/infmods.py verify extraoptions-garagerevving --zig /path/to/zig
python3 -I tools/infmods.py verify extraoptions-weathercontrol --zig /path/to/zig   # Windows (its systemImports)
```

The game files a mod needs (HUD, particles, shaders, sounds) are never in this repository or in its
packages: the manifest downloads them from the original author's release and checks each by SHA-256.

## Release

One GitHub release per drop, one ZIP per mod (`<id>-<version>.zip`, deterministic).

```sh
python3 -I tools/infmods.py release --zig /path/to/zig --tag <tag> --catalog catalog.json
```

writes `dist/assets/*.zip`, `dist/manifests/*.json`, `dist/catalog.json` and `dist/RELEASE.txt`.
After the release's assets are uploaded, `dist/catalog.json` replaces `catalog.json` in a pull request.
No entry requires `infinity-mod-runtime`; the release drops that retired entry from the catalog it writes.
A mod whose ZIP is byte-identical to the one its catalog entry already points to keeps that entry
unchanged (old URL), so players get no update; its ZIP may still be attached to the new release.
A mod whose ZIP changed needs a new `version` in `mod.json` and `manifest.json`, and its entry
points at the new release. The tool uploads nothing.

## Adding a mod

1. `git submodule add <original repository> upstream/<name>` and check out the exact release commit.
2. `mods/<id>/`: `mod.json`, `manifest.json`, the original `LICENSE`, a `NOTICE.md` naming the authors.
3. An ExtraOptions plugin itself needs no adapter (`adapter`: null); a recipe may add its upstream build's
   `extraFlags`, `forceInclude`, `systemImports` (import libraries of Windows' own DLLs) and a `generate` step.
   If the original loader has no ExtraOptions adapter yet, write one under `adapters/<loader>-eo/`
   (exports `FH2PluginInitialize`, ExtraOptions' `fh2_plugin.h`, ABI 1). Mods that need source
   changes (their own hook code, for example) use a fork as the submodule, with the changes as
   reviewed commits on top of the original.
4. `verify`, then a native check through the Infinity launcher before the catalog lists it.
