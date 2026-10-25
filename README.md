# Infinity Mods

Forza Horizon 2 mods adapted to run inside [Infinity Horizon](https://github.com/anarqz/InfinityHorizon)'s
mod runtime (`infinity_mod.dll`), with credit to the people who made them. Each mod is the
original author's source, pinned as a Git submodule and built unchanged, plus a small adapter that
connects the mod's original loader API to Infinity's.

| Mod | Original | Adapter | Licence |
|---|---|---|---|
| [Discord RPC](mods/discord-rpc) | [Bullet4fun/FH2_ModsCollection](https://github.com/Bullet4fun/FH2_ModsCollection) `discord-rpc/` | [FH2_ModLoader](adapters/fh2-modloader) | GPL-3.0-or-later |

Install them from the Infinity launcher's Mods page. They do not need the loaders they were
written for (FH2_ModLoader, FH2 Mod Platform); do not install both.

## Layout

```text
upstream/<name>/          original repositories, Git submodules pinned to an exact commit (never edited here)
adapters/<loader>/        one adapter per original loader API (MIT)
sdk/infinity_mod.h        Infinity mod SDK, ABI 1 (MIT, sdk/LICENSE-SDK.txt)
mods/<id>/mod.json        build recipe: which upstream sources, which adapter, DLL name, version
mods/<id>/manifest.json   Infinity manifest; the release fills package and file hashes and the commit
mods/<id>/LICENSE, NOTICE.md   the original licence and the attribution shipped in the package
tools/infmods.py          build, verify, release (Python 3.10+, standard library only)
```

## Build

[Zig 0.15.2](https://ziglang.org/download/) (the official archive, any OS) and Python 3.10+:

```sh
git submodule update --init
python3 -I tools/infmods.py build  discord-rpc --zig /path/to/zig   # build/FH2_DiscordRPC.dll
python3 -I tools/infmods.py verify discord-rpc --zig /path/to/zig   # two builds, same bytes; exports/imports
```

## Release

One GitHub release per drop, one ZIP per mod (`<id>-<version>.zip`, deterministic).

```sh
python3 -I tools/infmods.py release --zig /path/to/zig --tag <tag> --catalog <current catalog.json>
```

writes `dist/assets/*.zip`, `dist/manifests/*.json`, `dist/catalog.json` and `dist/RELEASE.txt`.
A mod whose ZIP is byte-identical to the one its catalog entry already points to keeps that entry
unchanged (old URL), so players get no update; its ZIP may still be attached to the new release.
A mod whose ZIP changed needs a new `version` in `mod.json` and `manifest.json`, and its entry
points at the new release. The tool uploads nothing.

## Adding a mod

1. `git submodule add <original repository> upstream/<name>` and check out the exact release commit.
2. `mods/<id>/`: `mod.json`, `manifest.json`, the original `LICENSE`, a `NOTICE.md` naming the authors.
3. If the original loader has no adapter yet, write one under `adapters/`. Mods that need source
   changes (their own hook code, for example) use a fork as the submodule, with the changes as
   reviewed commits on top of the original.
4. `verify`, then a native check through the Infinity launcher before the catalog lists it.
