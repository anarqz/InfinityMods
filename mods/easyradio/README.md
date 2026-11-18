# EasyRadio

Your own music on Forza Horizon 2's radio. Put folders of MP3s in `<Azimuth>\EasyRadio\`, pick which station plays
which folder, start the game: those stations play your songs, show their titles and artists on the HUD, show your
logo, shuffle at every start, and skip to the next song on a key or a pad tap. Only you hear it, so it works in every
mode.

Supported: FH2 on Azimuth 1.0.4 (`forza_durango_release_final.exe` sha256 `3ec04c61…7f0a3`) with ExtraOptions 0.6.x
(built into Infinity). EasyRadio is an ExtraOptions plugin: ExtraOptions' plugin loader loads it from the mod's
`plugins` folder.

## For players

### Install and set up

Install EasyRadio from the Infinity launcher's Mods page, then open its settings:

| Setting | Values | |
|---|---|---|
| Horizon Pulse, Bass Arena, Horizon XS, Radio Levante, Hospital Records, Ninja Tune, Innovative Leisure | `stock`, `music1` … `music8` | `stock` keeps the station's own songs; `musicN` plays `<Azimuth>\EasyRadio\musicN\` |
| DJ and stingers | `keep`, `off` | the station's DJ lines and stingers between your songs, or songs back to back |
| Skip key | `off`, `F1` … `F12` | default `F6`; ExtraOptions uses `F1`–`F4` |
| Skip pad button | `off`, `A` `B` `X` `Y` `LB` `RB` `LS` `RS` `View` `Menu` `Up` `Down` `Left` `Right` | default `LS` (a left-stick click is also the horn) |

Then put the MP3s in the folders, next to `azimuth.exe`:

```text
<Azimuth>\EasyRadio\
  music1\*.mp3        the songs of the station(s) set to music1
  music1\logo.png     optional: the station's HUD icon (any size, fitted into 196x96; transparency works; .jpg or .bmp too)
  music2\...
```

A folder that a station names but that does not exist yet is created, empty, at the next game start. Changes apply
at the next game start.

Your music stays outside the mod's own folder (`<Azimuth>\mods\Forza Horizon 2\easyradio\`), so updating,
disabling, uninstalling or reinstalling the mod never touches it; uninstalling keeps `easyradio.ini` too.

### By hand

`mods\Forza Horizon 2\easyradio\easyradio.ini` explains every option. Besides the launcher's values, a station
may name any folder of `<Azimuth>\EasyRadio\` (or of the mod's folder), or an absolute folder:

```ini
[radios]
horizon=music1            ; Horizon Pulse plays <Azimuth>\EasyRadio\music1
bass=D:\Music\dnb         ; any absolute folder
xs=stock                  ; the station keeps its own music
[options]
dj=keep
skip_key=F6
skip_pad=LS
```

The launcher's settings page only offers `stock` and `music1`–`music8`: a station set to another name or a path shows
"needs attention" there, and the launcher's "Repair settings" would set it back to `stock`.

Station ids: `horizon` (or `pulse`, Horizon Pulse), `bass` (Bass Arena), `xs` (Horizon XS), `levante` (Radio
Levante), `hospital` (Hospital Records), `ninjatune` (Ninja Tune), `innovative` (Innovative Leisure). The station keeps
its name: the game keys unlocks and icons by it.

- **Songs:** every `.mp3` in the folder (not in subfolders). Title and artist come from the ID3 tags (v2.2/2.3/2.4,
  or v1); without a title tag, from a file name `Artist - Title`. The HUD font is Latin-only, so accents are removed
  (`Beyoncé` → `Beyonce`), other scripts are dropped, video noise such as `(Official Music Video)` goes, `feat.`
  becomes `ft.`, and titles are cut at 80 characters, artists at 40.
- **Shuffle:** every game start plays the folder in a new random order.
- **Skip:** the key (only while the game window has the focus) or a lone tap of the pad button skips to the next
  song about 1.5 s later, with the station's stinger or DJ line as at a natural end. Spotify Radio also skips on
  `F6` and the left stick: with both installed, give EasyRadio other buttons.
- The first start after adding many songs reads them all; later starts use a cache.

### When something is wrong

Open `mods\Forza Horizon 2\easyradio\EasyRadio.log` (written at every start):

- `ready: hooks installed; N songs` and, a few seconds later, `RadioSystem::Load (language XX) ... reads the
  generated RadioInfo` mean it works.
- `SKIP <file>: <reason>`: that file is left out (not an MP3, shorter than 10 s, unreadable). The rest plays.
- `WARN: easyradio.ini line N: ...`: that line is ignored.
- `WARN: <station>: the folder ... did not exist; created it empty`: put MP3s in it.
- `FAIL: ... the radio stays stock`: nothing usable (every station `stock`, no playable song, or the game's files
  not found: then set `game_folder=` in `[options]` to the folder that holds `Mount`).
- `FAIL: the radio loaded before EasyRadio could start`: this start plays the stock radio; please report it.

A broken file never stops the game; with nothing valid the radio simply stays as it was.

### Limits

- One exact game build (Azimuth 1.0.4's FH2). Another build: the code checks refuse, the radio stays stock.
- MP3 only. Folders are not recursive. Up to 4000 songs per station.
- Another mod that replaces `RadioInfo_*.xml` or the radio icons through its `media` folder wins over EasyRadio.
- The HUD shows no station-name text; the name stays the station's.

## For maintainers

| Path | |
|---|---|
| `src/easyradio.c` | the plugin: ABI, code checks, the `RadioSystem::Load` gate, playback and skip hooks (Win32) |
| `src/er_gen.c`, `er_gen.h` | the generator: ini, folders, cache, shuffle, RadioInfo for 10 languages, icons |
| `src/er_ini.c`, `er_mp3.c`, `er_text.c`, `er_xml.c`, `er_logo.c`, `er_util.c`, `er_core.h` | the platform-free core |
| `src/er_platform_win32.c` | the core's file layer on Win32 (UTF-8 paths → UTF-16) |
| `src/er_unicode_tables.h` | generated by `gen_unicode.py` (NFKD folding, `\w`, `\s`, `str.lower`) |
| `third_party/stb_image.h` | stb_image 2.30 (public domain / MIT) |
| `tests/` | host unit tests of the core (`run_tests.py`; `--exe` also checks the hooked code against the game) |
| `easyradio.ini` | the settings file the package seeds (the launcher's form edits it) |

```sh
python3 -I tools/infmods.py verify easyradio --zig /path/to/zig            # two identical builds, exports, imports
python3 -I mods/easyradio/tests/run_tests.py --zig /path/to/zig [--exe /path/to/forza_durango_release_final.exe]
```

How it works, in short: at game start ExtraOptions calls `FH2PluginInitialize` on its loader thread. The plugin
checks the game code it touches (any difference: nothing is patched), swaps the `RadioSystem::Load` vtable slot for a
gate so the radio cannot read `RadioInfo_<lang>.xml` before the plugin has generated it, generates the edited
RadioInfo for every language and the HUD icons from the player's own game files into `plugins\generated\` (served as
this mod's media through `FH2PluginMediaDirectory`), and hooks FMOD's sound lookup so the stations' new entries play
the MP3s. EasyRadio must stay a mod without `database.json`, so ExtraOptions starts it before audio init.

Never commit music, logos, generated XML or HUD textures: they exist only on the player's machine.
