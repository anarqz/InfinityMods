# Nitro System · Infinity: attribution and scope

NitroSystem: qiangqiang101 (Bartholomew Ho), https://github.com/qiangqiang101/FH2-NitroSystem, MIT (LICENSE.txt,
from the original release). FH2 Mod Platform header: Trinculo54, https://github.com/Trinculo54/fh2-mod-platform, MIT
(LICENSE-FH2-Mod-Platform.txt). MinHook: Tsuda Kageyu, BSD-2-Clause (LICENSE-MinHook.txt). The exact commits are the
`upstream/nitrosystem` submodule (and its `platform` submodule) of https://github.com/anarqz/InfinityMods at the
commit this package was built from.

This package holds only the FH2 Mod Platform ExtraOptions adapter (`plugins\NitroSystem.Infinity.dll`; Alexandre
Moraes and contributors, MIT, LICENSE-Infinity.txt, with MinHook compiled in). NitroSystem itself is the author's
own release DLL, unchanged: Infinity downloads `FnFNitro-<version>.zip` from the author's GitHub release, checks it by
SHA-256 and installs `fh2-mods\FnFNitro.dll` and its licence into this folder (`<Azimuth>\mods\Forza Horizon
2\nitrosystem\`). ExtraOptions' mod loader (built into Infinity as ExtraOptions.Infinity) loads the adapter, which
starts the mod's `fh2mod_init`. The HUD, flame and garage files the mod hands to the game are written at each start
to `plugins\generated\`, this mod's private media; its INI (`fh2-mods\FnFNitro.ini`) and the NOS Kit of each car
(`data\titles\1A96545B\FnFNitro\player-1\FnFNitroGarage.json`) stay in this folder (shared by every player profile, and deleted with the folder when the mod is
uninstalled).

Not required: FH2 Mod Platform (`DWrite.dll`), the Infinity mod runtime or `d3dcompiler_47.dll`. Do not also place
FnFNitro.dll in `<Azimuth>\fh2-mods\`. Without the platform there is no in-game Mods page and no `fh2mod` commands:
the settings are in the Infinity launcher. The mods' interfaces work between the adapters' folders: with FH2 ANNA
installed, its Auto Drive switches NOS off while it drives.
