---
name: upstream-sync
description: Re-pin the official catalog's FH2 Mod Platform mods (NitroSystem, Underglow and any recipe with upstreamRelease) to their authors' newest GitHub releases - run tools/infmods.py sync, move the upstream submodule to the tag's commit, review what changed (platform API, INI keys, platform calls, files in the release ZIP), update the manifest's settings form, then hand the adapter build and release to WINDOWS. Use when Alex says a mod got an update ("o Nitro/Underglow atualizou", "pega a versão nova", "sync upstream", "update the mods") or asks to check the catalog's mods for new releases.
---

# Sync an FH2 Mod Platform mod with its author's new release

These mods (`mods/<id>/mod.json` with `"adapter": "fh2-mod-platform-eo"` and an `upstreamRelease` block) are the
**author's own release DLL**, never built here: their DLLs embed game-derived files (HUD XAML, camera presets,
textures), which must never be in our packages (the monorepo's AGENTS.md, rule 3). Our package holds only the adapter
(`adapters/fh2-mod-platform-eo`), which ExtraOptions loads from `plugins\` and which loads the author's DLL from
`<id>\<adapterConfig.modDll>` (for example `fh2-mods\FnFNitro.dll`). The manifest downloads the author's release ZIP
(`additionalPackages.upstream-assets`) and pins it, and each file taken from it, by SHA-256. An upstream update is
therefore a **re-pin plus a review**, not a port. Read `adapters/fh2-mod-platform-eo/README.md` once: it lists what the
adapter provides and what it does not.

Current mods: `nitrosystem` (qiangqiang101/FH2-NitroSystem, asset `FnFNitro-<v>.zip`) and `underglow`
(qiangqiang101/FH2-Underglow, asset `Underglow-<v>.zip`) and `fh2-anna` (qiangqiang101/FH2-ANNA, asset `FH2ANNA-<v>.zip`;
Underglow depends on it with an exact version, so a new ANNA version also needs Underglow's `dependencies` and a new
Underglow version).

## Rules

- Python with `-I`; downloads stay in memory (`sync`) or in an empty scratch folder; never run upstream scripts,
  build files or binaries. Never commit a release ZIP, a DLL or anything from it.
- Branch from `main` (`sync/<id>-<version>`); stage exact paths, never `git add -A`.
- Pushing a branch and opening a PR is fine when Alex asked for the update. **Publishing a GitHub release, uploading
  assets and merging a `catalog.json` change are Alex's to approve, each time** (`main` reaches every player at once).
- Building is WINDOWS' (MSVC). Game checks need Alex's explicit authorization in the conversation; never launch the game.
- One upstream version per mod per PR. Never "fix" the author's code or offsets here: a mod that needs a source change
  is not a re-pin (stop and ask).

## Steps

### 1. Report

```sh
git pull --rebase && git submodule update --init --recursive
python3 -I tools/infmods.py sync <id>               # latest release; --tag <tag> for a specific one
```

`sync` resolves the release, downloads the asset the recipe's `upstreamRelease.asset` matches, and reports the new
version (`<upstream>-infinity.1`, or the next `-infinity.<n>` when only the ZIP changed), its SHA-256 and size, the
DLL's exports and imports, and any **FAIL**. "already pinned" means there is nothing to do.

### 2. Fix what it reports

- **Submodule not at the tag's commit:** the command is in the message
  (`git -C upstream/<name> fetch --tags && git -C upstream/<name> checkout <sha>`), then
  `git -C upstream/<name> submodule update --init platform`. Re-run `sync`.
- **No entry matches a pattern** (the author renamed or moved a file in the ZIP): fix the pattern in
  `upstreamRelease.files` (or the asset name in `upstreamRelease.asset`), after checking the new ZIP's listing
  (`python3 -I -c "import zipfile,sys;print('\n'.join(zipfile.ZipFile(sys.argv[1]).namelist()))" <zip>`).
- **New imports** in the author's DLL: review them (Windows system DLLs only; never `DWrite.dll`, a platform DLL or
  another mod), then add them to `upstreamRelease.allowedImports`.
- **No `fh2mod_init`:** the mod left FH2 Mod Platform. Stop and ask Alex.
- **Platform API differs from the adapter's:** the author moved to a newer `fh2mod.h`. Stop the re-pin and update the
  adapter first, in its own commit:
  `git -C upstream/<name>/platform log --oneline <old>..<new> -- include/fh2mod.h` and its diff; add the new
  `FH2ModApi` fields to `fill_table()` (append-only, same order), implement or stub each (see the README's table),
  bump the `static_assert` and `platform_version`, and update the README. Get a fresh-context review of the adapter.

### 3. Review the upstream change (always, even when `sync` is clean)

```sh
git -C upstream/<name> log --oneline <pinned>..<new>
git -C upstream/<name> diff --stat <pinned>..<new>
grep -ohE "(g_)?api->[a-z_]+" -r upstream/<name>/src | sed 's/.*->//' | sort -u
```

Check, and write what you found in the PR description:

- **Platform calls:** every call in the list above is in the adapter's table. Calls the adapter answers with "not
  added" (`add_command`, `add_hotkey`, pages, string tables, `get_interface`...) are acceptable only if the mod still
  works without them (read the call site). `hook_pointer`, `patch_bytes` and `hook_instruction` are not supported:
  a mod that depends on them cannot be re-pinned.
- **`set_file` timing:** the adapter takes `set_file` only while `fh2mod_init` runs. A new `set_file` call made later
  (an INI watcher, a thread) is lost; find each call site and check it runs inside `fh2mod_init`.
- **Files outside the DLL:** `sync` excludes every ZIP entry the recipe does not map. If the mod now needs a file from
  its ZIP (a sound, an INI default, a folder), add a pattern to `upstreamRelease.files` and the file to the manifest
  (`role` asset, `package` upstream-assets) under the mod's folder.
- **Paths:** the mod builds paths from `azimuth_dir` (= its own mod folder here) and `game_dir` (empty here). Any new
  write outside those (the real saves, `mediaoverride`, the game folder) blocks the re-pin: ask Alex.
- **Online behaviour:** a new `offline_only` or `playing_online` use changes nothing here (the manifest's multiplayer
  impact decides: not cosmetic = Noob only), but say so in the PR.

### 4. Settings form

`sync` compares the INI keys the mod reads with `config.options`:

- **FAIL "whose key is nowhere in the mod's sources":** remove that option (and its id from `multiplayer.settings`).
- **note "INI keys the mod reads that the settings form does not offer":** add each one a player should see, with the
  mod's own range, default and wording. pt-BR comes from the mod's translation table (`src/lang.cpp` or
  `src/neon_text.h`, the "BR" column); write it yourself only when the mod has none. Every number's default must be
  `minimum + k * step`; enums have at most 64 choices; `apply` is `next-launch` (no in-game Mods page here). Add
  gameplay options (anything other players race against) to `multiplayer.settings` and bump `multiplayer.revision`.
  Legacy keys the mod only migrates or deletes stay out of the form.
- Defaults that changed upstream: update `default` to the mod's new one.

### 5. Write and check

```sh
python3 -I tools/infmods.py sync <id> --write   # mod.json, manifest.json, mods/<id>/extraoptions/mod.json
python3 -I tools/infmods.py check
```

Then update `README.md`'s table if the repository or name changed, and the mod's `NOTICE.md` if the files or paths
changed. Commit `mods/<id>/` and `upstream/<name>` (the submodule pointer) together:
`<Mod> <new version> from <owner>/<repo> <tag>`.

### 6. Build, release, catalog (WINDOWS builds; Alex approves the publishing steps)

On WINDOWS (Visual Studio C++ x64 tools):

```sh
python3 -I tools/infmods.py verify <id>
python3 -I tools/infmods.py release --tag v<YYYY.MM.DD> --catalog catalog.json <id> [<id> ...]
```

`release` builds the adapter, packs `<id>-<version>.zip`, fills the manifest (package hash, source commit) and writes
`dist/catalog.json`. With Alex's approval: create the GitHub release `v<YYYY.MM.DD>` with `dist/assets/*.zip`,
download the assets back and compare their SHA-256 with `dist/RELEASE.txt`, then open a PR replacing `catalog.json`
with `dist/catalog.json` (and committing `dist/manifests/<id>-<version>.json` over `mods/<id>/manifest.json` if you keep
the filled copy). Alex merges.

### 7. Native check and review status

With Alex's authorization only: install from the PR's catalog in a DEV launcher, start the game once, and read
`FH2_ExtraOptions.log` for `FH2 Mod Platform adapter: started`, `private media directory ready` (when the mod hands
files over) and `the mod's SQL is applied`. Then set `review.status` to `reviewed` (blockers `[]`) in the manifest
before the catalog PR merges.

## Done means

`sync` reports "already pinned" for the new release on `main`; the PR states the upstream range reviewed, the
platform calls checked, the settings changed and the release/asset hashes; Alex approved the release and merged the
catalog.
