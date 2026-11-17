// MIT; see LICENSE. The FH2 Mod Platform's translations (API 10) under ExtraOptions.
//
// The mod's own platform/src/l10n.cpp, at the commit the mod was built against, is compiled into the adapter
// unchanged (the recipe's upstream.sources): PO catalogues, tr() / tr_plural() with their fallbacks, plural
// rules, number and date formats, the language memory and the test language. This file is the rest of the
// platform that l10n.cpp calls, as the platform's own tests/l10n_test.cpp provides it:
//   - fh2platform::azimuth_dir: the mod's own folder (as the table's azimuth_dir), so the language memory is
//     <mod>\fh2-mods\platform-language.ini and a translator's file <mod>\fh2-mods\lang\<mod name>\<lang>.po;
//   - fh2platform::log_line: ExtraOptions' log;
//   - fh2platform::game_language: the game's text language, read as the platform's lang.cpp reads it (the
//     string tables' manager at game+0x3662990, its code an MSVC std::string at +0x30), from the image
//     ExtraOptions hands the adapter;
//   - fh2platform::launched_game: FH2 (ExtraOptions loads the adapter only into it);
//   - fh2platform::cut_text: as the platform's text.cpp (UTF-16 units), for l10n.cpp's link only.
// And the mod's record: struct FH2Mod is the platform's (platform/src/platform.hpp), of which l10n.cpp reads the
// name, so the adapter hands the mod that record as its FH2Mod*.
#include "fh2_mod_platform_eo_l10n.h"

#include "platform.hpp"   // the platform's (platform/src): struct FH2Mod and what l10n.cpp declares
#include "l10n.hpp"
#include "lang.hpp"
#include "text.hpp"

#include <windows.h>

#include <cctype>
#include <cstring>
#include <string>

namespace {
FH2Mod g_record;
void (*g_log)(const char* line) = nullptr;
std::wstring g_dir;
std::string g_who = "FH2 Mod Platform adapter";   // the platform's own log lines: with the mod's folder
const uint8_t* g_game = nullptr;
size_t g_game_size = 0;
constexpr uintptr_t kStringTables = 0x3662990;   // the platform's RVA_STRING_TABLES (lang.cpp), the mods' too

// A read of memory that may not be there: false instead of a fault.
bool copy_from_game(void* to, const void* from, size_t n) {
#ifdef _MSC_VER
    __try {
        std::memcpy(to, from, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else   // a host test (tests/l10n_test.cpp): its fake image is all there
    std::memcpy(to, from, n);
    return true;
#endif
}

// The length of the UTF-8 character at s[i], with its UTF-16 units in `units`; 0 for a byte that doesn't start one.
size_t utf8_at(const std::string& s, size_t i, size_t& units) {
    const auto c = static_cast<unsigned char>(s[i]);
    const size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (!n || i + n > s.size()) { units = 1; return 0; }
    for (size_t k = 1; k < n; ++k)
        if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) { units = 1; return 0; }
    units = n == 4 ? 2 : 1;
    return n;
}
} // namespace

namespace fh2platform {
void log_line(const std::string& who, const std::string& text) {
    if (!g_log) return;
    const std::string line = (who.empty() ? g_who : who) + ": " + text + "\r\n";
    g_log(line.c_str());
}

std::filesystem::path azimuth_dir() { return g_dir; }

Launched launched_game() { return Launched::fh2; }

int game_language(char* code, size_t size) {
    if (!code || size < 4) return 0;
    code[0] = '\0';
    // The FH2 build the RVA belongs to (the platform's is_game checks the same size); a size of 0: not given, the reads guard
    if (!g_game || (g_game_size && g_game_size != kGameSizeOfImage)) return 0;
    const uint8_t* manager = nullptr;
    uint8_t s[0x20];
    if (!copy_from_game(&manager, g_game + kStringTables, sizeof manager) || !manager || !copy_from_game(s, manager + 0x30, sizeof s)) return 0;
    uint64_t length = 0, capacity = 0;
    std::memcpy(&length, s + 0x10, sizeof length);
    std::memcpy(&capacity, s + 0x18, sizeof capacity);
    if (capacity != 15 || length < 2 || length > 3 || length >= size || s[length]) return 0;
    for (uint64_t i = 0; i < length; ++i) {
        if (!std::isalpha(s[i])) return 0;
        code[i] = static_cast<char>(std::toupper(s[i]));   // the zips are "br", "nl", "cz"...: the codes in capitals
    }
    code[length] = '\0';
    return 1;
}

std::string cut_text(const std::string& s, std::size_t chars) {
    std::size_t i = 0, used = 0, units = 0;
    while (i < s.size()) {
        std::size_t n = utf8_at(s, i, units);
        if (!n) n = 1;
        if (used + units > chars) break;
        used += units;
        i += n;
    }
    return s.substr(0, i);
}
} // namespace fh2platform

FH2Mod* eo_l10n_mod() { return &g_record; }

void eo_l10n_start(const wchar_t* mod_dir, const void* game_image, size_t game_image_size, void (*log)(const char* line)) {
    g_log = log;
    g_dir = mod_dir ? mod_dir : L"";
    const std::wstring folder = std::filesystem::path(g_dir).filename().wstring();
    const int n = WideCharToMultiByte(CP_UTF8, 0, folder.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n > 1) {
        std::string name(static_cast<size_t>(n - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, folder.c_str(), -1, name.data(), n, nullptr, nullptr);
        g_who = "FH2 Mod Platform adapter (" + name + ")";
    }
    g_game = static_cast<const uint8_t*>(game_image);
    g_game_size = game_image_size;
    fh2platform::l10n_start();   // reads the language memory, logs where mods' text starts, watches for the game's
}

void eo_l10n_register(const char* name, const char* version, const char* description) {
    g_record.name = name ? name : "";
    g_record.version = version ? version : "";
    g_record.description = description ? description : "";
}
