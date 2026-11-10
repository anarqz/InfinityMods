# Spotify Radio: attribution and scope

Spotify Radio: Bullet4fun, part of FH Mods Collection, https://github.com/Bullet4fun/FH_ModsCollection,
GPL-3.0-or-later (LICENSE.txt). Built from upstream's source (`spotify-radio/`, FH Mods Collection 2.3.0,
commit 5445c692) with Infinity's reviewed commits on top, at the exact commit of the
`upstream/fh-mods-collection` submodule of https://github.com/anarqz/InfinityMods at the commit this package
was built from, with Zig's Clang and the flags of upstream's own `build.py` plus `-DINFINITY_EO=1`. What
those commits change is in that source's `infinity/README.md`: an ExtraOptions plugin entry
(`FH2PluginInitialize`) instead of the Azimuth Mod Loader's, hooks written with the game's other threads
suspended, and no files written outside this folder.

ExtraOptions' mod loader (built into Infinity as ExtraOptions.Infinity) loads `plugins\FH_SpotifyRadio.dll`
from this folder (`<Azimuth>\mods\Forza Horizon 2\spotifyradio\`). `librespot.exe` (librespot 0.8.0 with
Bullet4fun's patches, MIT, LICENSE-librespot.txt), `onevent.cmd` and the station logo are not in this
package: Infinity downloads them from Bullet4fun's own release (FH_SpotifyRadio.zip, v2.3.0) and checks each by
SHA-256. Spotify Radio plays Spotify (Premium) on the game's Xbox Music station; your Spotify login is saved in
this folder's `credentials.json`. Nothing other players see, hear or race against.

Not affiliated with Spotify.
