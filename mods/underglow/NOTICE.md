# Underglow · Infinity: attribution and scope

Underglow: qiangqiang101 (Bartholomew Ho), https://github.com/qiangqiang101/FH2-Underglow, MIT (LICENSE.txt, from the
original release). FH2 Mod Platform header: Trinculo54, https://github.com/Trinculo54/fh2-mod-platform, MIT
(LICENSE-FH2-Mod-Platform.txt). MinHook: Tsuda Kageyu, BSD-2-Clause (LICENSE-MinHook.txt). The exact commits are the
`upstream/underglow` submodule (and its `platform` submodule) of https://github.com/anarqz/InfinityMods at the commit
this package was built from.

This package holds only the FH2 Mod Platform ExtraOptions adapter (`plugins\Underglow.Infinity.dll`; Alexandre
Moraes and contributors, MIT, LICENSE-Infinity.txt, with MinHook compiled in). Underglow itself is the author's own
release DLL, unchanged: Infinity downloads `Underglow-<version>.zip` from the author's GitHub release, checks it by
SHA-256 and installs `fh2-mods\Underglow.dll` and its licence into this folder (`<Azimuth>\mods\Forza Horizon
2\underglow\`). ExtraOptions' mod loader (built into Infinity as ExtraOptions.Infinity) loads the adapter, which
starts the mod's `fh2mod_init`. The garage camera shots and Underglow Kit pictures the mod hands to the game are
written at each start to `plugins\generated\`, this mod's private media; its INI (`fh2-mods\Underglow.ini`) and the
kit, colors and effects of each car (`data\titles\1A96545B\Underglow\player-1\UnderglowGarage.json`) stay in this
folder (shared by every player profile, and deleted with the folder when the mod is
uninstalled).

Buying an Underglow Kit goes through the game's own shop, so the save records the part (part type 48); that entry
stays in the save if the mod is removed. Not required: FH2 Mod Platform (`DWrite.dll`) or the Infinity mod runtime.
Do not also place Underglow.dll in `<Azimuth>\fh2-mods\`. Without the platform there is no in-game Mods page, no
`fh2mod` commands and no FH2 ANNA menus: the settings are in the Infinity launcher.
