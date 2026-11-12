# FH2 ANNA · Infinity: attribution and scope

FH2 ANNA: qiangqiang101 (Bartholomew Ho), https://github.com/qiangqiang101/FH2-ANNA, MIT (LICENSE.txt, from the
original release). FH2 Mod Platform header: Trinculo54, https://github.com/Trinculo54/fh2-mod-platform, MIT
(LICENSE-FH2-Mod-Platform.txt). MinHook: Tsuda Kageyu, BSD-2-Clause (LICENSE-MinHook.txt). The exact commits are the
`upstream/anna` submodule (and its `platform` submodule) of https://github.com/anarqz/InfinityMods at the commit this
package was built from.

This package holds only the FH2 Mod Platform ExtraOptions adapter (`plugins\FH2ANNA.Infinity.dll`; Alexandre Moraes
and contributors, MIT, LICENSE-Infinity.txt, with MinHook compiled in). FH2 ANNA itself is the author's own release
DLL, unchanged: Infinity downloads `FH2ANNA-<version>.zip` from the author's GitHub release, checks it by SHA-256 and
installs `fh2-mods\FH2ANNA.dll` and its licence into this folder (`<Azimuth>\mods\Forza Horizon 2\fh2-anna\`).
ExtraOptions' mod loader (built into Infinity as ExtraOptions.Infinity) loads the adapter, which starts the mod's
`fh2mod_init`. The HUD textures the mod hands to the game are written at each start to `plugins\generated\`, this
mod's private media; its INI (`fh2-mods\FH2ANNA.ini`) stays in this folder (shared by every player profile, and
deleted with the folder when the mod is uninstalled).

Other mods add to ANNA's menu through its interface (Underglow: Car > Lights and Car > Underglow), and ANNA's Auto
Drive switches NitroSystem's NOS off while it drives; the adapters share those interfaces between the mods' folders.
Not required: FH2 Mod Platform (`DWrite.dll`) or the Infinity mod runtime. Do not also place FH2ANNA.dll in
`<Azimuth>\fh2-mods\`. Without the platform there is no in-game Mods page: the settings are in the Infinity launcher.
