# Discord RPC · Infinity: attribution and scope

Discord RPC: Bullet4fun, FH2_ModsCollection (https://github.com/Bullet4fun/FH2_ModsCollection),
GPL-3.0-or-later (LICENSE.txt). The exact upstream commit is the `upstream/fh2-mods-collection`
submodule of https://github.com/anarqz/InfinityMods at the commit this package was built from.

Built unchanged from upstream's sources, plus the FH2_ModLoader ExtraOptions adapter (Alexandre Moraes
and contributors, MIT, LICENSE-Infinity.txt), which lets ExtraOptions' mod loader (built into Infinity
as ExtraOptions.Infinity) start it instead of FH2_ModLoader. ExtraOptions loads
`plugins\FH2_DiscordRPC.dll` from this folder (`<Azimuth>\mods\discord-rpc\`); `[Infinity] Enabled=0`
in `FH2_DiscordRPC.ini` beside it keeps it off. The combined DLL is distributed under
GPL-3.0-or-later; its complete source is that repository at that commit.

Not required: FH2_ModLoader (`data\bin\azimuth-tls.dll`) or the Infinity mod runtime. Do not also
place FH2_DiscordRPC.dll in `data\bin\mods\` or `infinity-mods\DiscordRPC\`.
