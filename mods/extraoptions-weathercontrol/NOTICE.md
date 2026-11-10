# Extra Options - Weather Control: attribution and scope

Weather Control: BlackPanthaa, adapted from Set Time & Weather FH2 by Pantsedit (GPL-3.0), with Extra Options
integration by Arthur (RokkuDayo), https://github.com/RokkuDayo/ForzaHorizon2_ExtraOptions, GPL-3.0
(LICENSE.txt; CREDITS.md is upstream's). Built unchanged from upstream's source (`fh2/plugins/weather-control`),
at the exact commit of the `upstream/extraoptions` submodule of https://github.com/anarqz/InfinityMods at the
commit this package was built from, with Zig's Clang, the flags of upstream's own `build.py` and its native
pages generated from its `translations.json` and `weather_page.template.xaml` the same way; only the version
resource is Infinity's.

ExtraOptions' mod loader (built into Infinity as ExtraOptions.Infinity) loads `plugins\EO_WeatherControl.dll`
from this folder (`<Azimuth>\mods\Forza Horizon 2\extraoptions-weathercontrol\`). It changes the time and the
weather of the world you drive in, so Infinity offers it to Noob profiles only.
