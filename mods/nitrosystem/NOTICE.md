# Nitro System · Infinity: attribution and scope

NitroSystem: qiangqiang101 (Bartholomew Ho), https://github.com/qiangqiang101/NitroSystem, MIT
(LICENSE.txt). FH2 Mod Platform header: Trinculo54, https://github.com/Trinculo54/fh2-mod-platform,
MIT (LICENSE-FH2-Mod-Platform.txt). The exact commits are the `upstream/nitrosystem` submodule (and
its `platform` submodule) of https://github.com/anarqz/InfinityMods at the commit this package was
built from.

Built unchanged from upstream's source, plus the FH2 Mod Platform adapter and the Infinity mod SDK
(Alexandre Moraes and contributors, MIT, LICENSE-Infinity.txt), which let Infinity's mod runtime
start it instead of FH2 Mod Platform. Upstream's game files (HUD, particles, shaders) and sounds are
not in this package: Infinity downloads them from upstream's own release and checks each one.

Not required: FH2 Mod Platform or `d3dcompiler_47.dll`. Do not also place FnFNitro.dll in `fh2-mods\`.
