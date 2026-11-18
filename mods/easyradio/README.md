# EasyRadio

Your own music on Forza Horizon 2's radio. Put folders of MP3s in `<Azimuth>\EasyRadio\`, pick which station plays
which folder, start the game: those stations play your songs, show their titles and artists on the HUD, show your
logo, shuffle at every start, and skip to the next song on a key or a pad tap. Only you hear it, so it works in every
mode.

Supported: FH2 on Azimuth 1.0.4 (`forza_durango_release_final.exe` sha256 `3ec04c61…7f0a3`) with ExtraOptions 0.6.x
(built into Infinity). EasyRadio is an ExtraOptions plugin: ExtraOptions' plugin loader loads it from the mod's
`plugins` folder.

## For players

### Quick start (5 minutes)

1. **Install EasyRadio** from the Infinity launcher: **Mods** page → **EasyRadio** → **Install**.
2. **Make a music folder** next to `azimuth.exe` (the Azimuth folder you chose in the launcher):

   ```text
   <Azimuth>\
     azimuth.exe
     EasyRadio\           create this folder
       music1\            and this one inside it
         My Song.mp3
         Another Song.mp3
         logo.png         optional: your station's logo
   ```

   Not inside `mods\`: the music folder sits beside `mods\`, so updating or uninstalling the mod never deletes your
   songs.
3. **Pick the station:** on the Mods page, open EasyRadio's **Options** and set, for example, **Horizon Pulse** to
   `music1`. Leave the other stations on `stock` to keep their own songs.
4. **Start the game** and tune to that station. Your songs play in a new random order at every start, with their
   titles and artists on the radio HUD and your logo as the station icon.
5. **Skip a song:** press **F6**, or quickly tap the **left stick** (a short tap, under half a second). About a
   second and a half later the next song starts.

Changes (new songs, a new logo, other settings) apply the next time the game starts.

### More stations

You have eight folders to use: `music1` … `music8`. Each station can play any of them, and two stations can share
one. For example: Horizon Pulse → `music1` (your pop), Bass Arena → `music2` (your drum and bass). A folder a
station names but that does not exist yet is created, empty, at the next game start, so you can also set the
station first and fill the folder afterwards.

### Your songs

- **MP3 files only**, directly in the folder (subfolders are not read). Files shorter than 10 seconds are skipped.
- **Title and artist** come from the file's tags (ID3, as music players and taggers write them). A file without
  tags uses its name, written as `Artist - Title.mp3`.
- The game's radio font has only Latin letters: accents are removed (`Beyoncé` shows as `Beyonce`) and other
  scripts (Japanese, Cyrillic, ...) are left out of the HUD text. The song still plays.
- Video leftovers such as `(Official Music Video)` are cleaned from titles; `feat.` shows as `ft.`.
- Long songs play to the end. Up to 4000 songs per station. The first start after adding many songs takes a moment
  longer while EasyRadio reads them; later starts use a cache.

### Your logo

Put `logo.png` (or `logo.jpg`, `logo.bmp`) in the music folder. Any size works: it is fitted into the station icon,
which is about twice as wide as it is tall (196 × 96), so a wide image looks best. Transparent PNGs work. Without a
logo the station keeps its own icon. The station's name stays the same (the game ties unlocks to it).

### Settings

| Setting | Values | What it does |
|---|---|---|
| Horizon Pulse, Bass Arena, Horizon XS, Radio Levante, Hospital Records, Ninja Tune, Innovative Leisure | `stock`, `music1` … `music8` | `stock` keeps the station's own songs; `musicN` plays `<Azimuth>\EasyRadio\musicN\` |
| DJ and stingers | `keep`, `off` | `keep`: the station's DJ lines and jingles play between your songs. `off`: your songs back to back |
| Skip key | `off`, `F1` … `F12` | default `F6` (works while the game window has the focus). Extra Options already uses `F1`–`F4` |
| Skip pad button | `off`, `A` `B` `X` `Y` `LB` `RB` `LS` `RS` `View` `Menu` `Up` `Down` `Left` `Right` | default `LS`, a quick tap only. A left-stick click is also the horn |

If you also use **Spotify Radio**, it uses `F6` and the left stick too: give EasyRadio other buttons.

### Questions

- **The station still plays its own songs.** Check that the station is set to a folder (not `stock`), that the folder
  is `<Azimuth>\EasyRadio\musicN\` and holds `.mp3` files, and that you restarted the game. Then read the log below.
- **The icon did not change.** The folder has no `logo.png` / `logo.jpg` / `logo.bmp`.
- **A song is missing.** It is not an MP3, is shorter than 10 seconds, or could not be read: the log names it.
- **Online / Hardcore?** EasyRadio is allowed in every mode: only you hear your radio.

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
Levante), `hospital` (Hospital Records), `ninjatune` (Ninja Tune), `innovative` (Innovative Leisure).

### The log

`mods\Forza Horizon 2\easyradio\EasyRadio.log` is written at every game start:

- `ready: hooks installed; N songs` and, a few seconds later, `RadioSystem::Load (language XX) ... reads the
  generated RadioInfo` mean it works.
- `pulse: HUD icon made from ...logo.png`: your logo is in use.
- `SKIP <file>: <reason>`: that file is left out. The rest plays.
- `WARN: easyradio.ini line N: ...`: that line is ignored.
- `WARN: <station>: the folder ... did not exist; created it empty`: put MP3s in it.
- `FAIL: ... the radio stays stock`: nothing usable (every station `stock`, no playable song, or the game's files
  not found: then set `game_folder=` in `[options]` to the folder that holds `Mount`).
- `FAIL: the radio loaded before EasyRadio could start`: this start plays the stock radio; please report it.

A broken file never stops the game; with nothing valid the radio simply stays as it was.

### Limits

- One exact game build (Azimuth 1.0.4's FH2). On another build the code checks refuse and the radio stays stock.
- MP3 only; folders are not recursive; up to 4000 songs per station.
- Another mod that replaces `RadioInfo_*.xml` or the radio icons through its `media` folder wins over EasyRadio.

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
