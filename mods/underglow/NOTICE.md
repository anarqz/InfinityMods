# Underglow: attribution and scope

Underglow: qiangqiang101 (Bartholomew Ho), https://github.com/qiangqiang101/FH2-Underglow, MIT (LICENSE.txt, the
original repository's licence). FH2 Mod Platform header: Trinculo54, https://github.com/Trinculo54/fh2-mod-platform, MIT
(LICENSE-FH2-Mod-Platform.txt). MinHook: Tsuda Kageyu, BSD-2-Clause (LICENSE-MinHook.txt). The exact commits are the
`upstream/underglow` submodule (and its `platform` submodule) of https://github.com/anarqz/InfinityMods at the commit
this package was built from.

**Patched to coexist with the Infinity hook.** `fh2-mods\Underglow.dll` in this package is built by Infinity Mods from
the author's source at the pinned commit with two patches (`mods/underglow/patches/` in InfinityMods). 0001: Underglow checks the stock bytes of the game's car update before hooking it, and the Infinity hook
(online headlight and roof sync) and NitroSystem hook that function too, so the original stayed off under Infinity.
The patched check also accepts another hook's jump there (E9 rel32, or FF 25 with its address) and hooks over it.
0002 (online, under Infinity only): Underglow finds the Infinity hook's car-state sync (`InfinityCarSync_*`, looked up
with GetProcAddress), gives it this car's look as a 40-byte blob (on/off, parts, LED level, colors, effects, shape) and
shows the neon of the other players in the same world who have Underglow too: the 8 nearest within 150 m, at most 32
lights each, audio effects as a steady glow. Nothing else is sent. Without the Infinity hook it does nothing. 0002
compiles in the hook's API header `infinity_carsync.h` as it is, with its notice: Alexandre Moraes and the Infinity
Launcher contributors, MIT (Infinity Horizon, https://github.com/anarqz/InfinityHorizon, `native/hook/include`; the
rest of that hook is GPL-3.0-or-later and none of it is in this package). Everything
else is the author's code, unchanged. That DLL contains the author's `camera_presets.h`, his edited copy of
the game's garage camera shots with his five Underglow shots, shipped here by the owner's decision (Alex, 2026-11-12,
an exception to the rule that no game-derived material ships; nothing else game-derived is included).

The package also holds the FH2 Mod Platform ExtraOptions adapter (`plugins\Underglow.Infinity.dll`; Alexandre Moraes
and contributors, MIT, LICENSE-Infinity.txt, with MinHook compiled in), installed in `<Azimuth>\mods\Forza Horizon
2\underglow\`. ExtraOptions' mod loader (built into Infinity as ExtraOptions.Infinity) loads the adapter, which starts
the mod's `fh2mod_init`. The garage camera shots and Underglow Kit pictures the mod hands to the game are written at
each start to `plugins\generated\`, this mod's private media; its INI (`fh2-mods\Underglow.ini`) and the kit, colors
and effects of each car (`data\titles\1A96545B\Underglow\player-1\UnderglowGarage.json`) stay in this folder (shared by
every player profile, and deleted with the folder when the mod is uninstalled).

Buying an Underglow Kit goes through the game's own shop, so the save records the part (part type 48); that entry
stays in the save if the mod is removed. Requires FH2 ANNA (the `fh2-anna` mod, loaded first): Underglow adds Car >
Lights > Underglow and Car > Underglow (colors, effects and parts of each car) to ANNA's menu through ANNA's interface,
which the adapters share between the two folders. Not required: FH2 Mod Platform (`DWrite.dll`) or the Infinity mod
runtime. Do not also place Underglow.dll in `<Azimuth>\fh2-mods\`. Without the platform there is no in-game Mods page
and no `fh2mod` commands: the settings are in the Infinity launcher.
