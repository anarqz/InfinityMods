# Nitro System · Infinity: attribution and scope

NitroSystem: qiangqiang101 (Bartholomew Ho), https://github.com/qiangqiang101/NitroSystem, MIT
(LICENSE.txt). FH2 Mod Platform header: Trinculo54, https://github.com/Trinculo54/fh2-mod-platform,
MIT (LICENSE-FH2-Mod-Platform.txt). The exact commits are the `upstream/nitrosystem` submodule (and
its `platform` submodule) of https://github.com/anarqz/InfinityMods at the commit this package was
built from.

Built unchanged from upstream's source, plus the FH2 Mod Platform ExtraOptions adapter (Alexandre Moraes
and contributors, MIT, LICENSE-Infinity.txt) and MinHook (Tsuda Kageyu, BSD-2-Clause, LICENSE-MinHook.txt;
the copy in upstream's `platform/third_party/minhook`). ExtraOptions' mod loader (built into Infinity as
ExtraOptions.Infinity) loads `plugins\FnFNitro.dll` from this folder (`<Azimuth>\mods\nitrosystem\`); the
mod's INI and sounds stay in the same folder. Upstream's game files (HUD, particles, shaders) and sounds are
not in this package: Infinity downloads them from upstream's own release and checks each one; they go to
this folder's `media\`, which ExtraOptions mounts over the game's media.

Not required: FH2 Mod Platform, the Infinity mod runtime or `d3dcompiler_47.dll`. Do not also place
FnFNitro.dll in `fh2-mods\`.
