# Camera Mods: attribution and scope

Camera Mods: Deuquan, https://github.com/Deuquan/FH2-CameraMods, GPL-3.0 (LICENSE.txt), itself built on
RokkuDayo's Extra Options (https://github.com/RokkuDayo/ForzaHorizon2_ExtraOptions, GPL-3.0). Built unchanged
from upstream's source (`plugin/plugin_main.c`, `features/drone_probe.c`, `features/overlay_window.c`, the
sources of its `build_plugin.py`), at the exact commit of the `upstream/fh2-cameramods` submodule of
https://github.com/anarqz/InfinityMods at the commit this package was built from, with Zig's Clang and the
flags of upstream's own `build.py`; only the version resource is Infinity's.

ExtraOptions' mod loader (built into Infinity as ExtraOptions.Infinity) loads `plugins\CameraMods.dll` from
this folder (`<Azimuth>\mods\Forza Horizon 2\camera-mods\`). Left out: upstream's optional
`CameraModsMenu.dll`, an FH2 Mod Platform page for rebinding keys; without it the mod keeps its default keys.
A free and drone camera can show more than the game's own cameras in a race, so it plays in Noob only.
