// MIT; see LICENSE. The adapter's side of the platform's translations (fh2_mod_platform_eo_l10n.cpp).
#pragma once

#include <cstddef>

struct FH2Mod;

// The mod's record (the platform's struct FH2Mod): what register_mod returns.
FH2Mod* eo_l10n_mod();
// Before fh2mod_init: the mod's folder (azimuth_dir), the FH2 image ExtraOptions handed over, and the log.
void eo_l10n_start(const wchar_t* mod_dir, const void* game_image, size_t game_image_size, void (*log)(const char* line));
// register_mod's FH2ModInfo: the name finds a translator's fh2-mods\lang\<name>\ and goes on the log lines.
void eo_l10n_register(const char* name, const char* version, const char* description);
