// MIT; see ../LICENSE. The adapter's translations on a host: fh2_mod_platform_eo_l10n.cpp and the mod's own
// platform/src/l10n.cpp, through an FH2ModApi filled as fill_table() fills it, with the mod's built-in
// translations (TRANSLATIONS_INC, TRANSLATIONS) and a fake FH2 image whose string tables' manager holds the
// game's language. run_tests.py builds and runs it per mod:
//   l10n_test <empty folder> <mod name> <key> <English> <pt-BR text>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "fh2mod.h"
#include "fh2mod_l10n.h"
#include "l10n.hpp"
#include "lang.hpp"
#include "platform.hpp"   // kGameSizeOfImage
#include "fh2_mod_platform_eo_l10n.h"
#include TRANSLATIONS_INC

static int fails = 0;
#define CHECK(x) do { const bool ok_ = (x); std::printf("%s %s\n", ok_ ? "ok  " : "FAIL", #x); if (!ok_) ++fails; } while (0)

static void log_line(const char* line) { std::printf("  log: %s", line); }

constexpr size_t kStringTables = 0x3662990;
static std::vector<uint8_t> image(kStringTables + 64);
static uint8_t manager[0x60];
// The game picking its language: the manager's code, an MSVC std::string at +0x30 (SSO: text, size, capacity 15).
static void set_game_language(const char* code) {
    uint8_t* p = manager;
    std::memcpy(image.data() + kStringTables, &p, sizeof p);
    std::memset(manager + 0x30, 0, 0x20);
    std::memcpy(manager + 0x30, code, std::strlen(code));
    const uint64_t length = std::strlen(code), capacity = 15;
    std::memcpy(manager + 0x40, &length, 8);
    std::memcpy(manager + 0x48, &capacity, 8);
}

static std::string read(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

int main(int argc, char** argv) {
    if (argc != 6) { std::printf("usage: l10n_test <folder> <mod name> <key> <English> <pt-BR>\n"); return 2; }
    const std::string folder = argv[1], key = argv[3], english = argv[4], brazilian = argv[5];
    const std::wstring dir(folder.begin(), folder.end());

    FH2ModApi api{};
    api.size = sizeof api;
    api.api_version = FH2MOD_API_VERSION;
    api.game_language = fh2platform::game_language;
    api.language = fh2platform::l10n_language;
    api.add_translation = fh2platform::l10n_add_translation;
    api.tr = fh2platform::l10n_tr;
    api.tr_plural = fh2platform::l10n_tr_plural;
    api.format_number = fh2platform::l10n_format_number;
    api.format_date = fh2platform::l10n_format_date;

    // First launch: no language memory, Windows says pt-BR (TEST_LOCALE), the game has no language yet.
    eo_l10n_start(dir.c_str(), image.data(), fh2platform::kGameSizeOfImage, log_line);   // only its first bytes are read
    FH2Mod* mod = eo_l10n_mod();
    eo_l10n_register(argv[2], "1.0.0", "test");
    char code[8];
    CHECK(api.game_language(code, sizeof code) == 0);
    CHECK(std::string(api.language()) == "pt-BR");
    const size_t languages = sizeof(TRANSLATIONS) / sizeof(TRANSLATIONS[0]);
    CHECK(FH2_ADD_TRANSLATIONS(&api, mod, TRANSLATIONS) == static_cast<int>(languages));
    const char* first = FH2_TR(&api, mod, key.c_str(), english.c_str());
    CHECK(first == brazilian);
    CHECK(FH2_TR(&api, mod, key.c_str(), english.c_str()) == first);   // the same copy each time

    // The game picks Brazilian Portuguese, then Czech (English text: no catalogue), then English.
    set_game_language("br");
    CHECK(api.game_language(code, sizeof code) == 1 && std::string(code) == "BR");
    CHECK(std::string(api.language()) == "pt-BR");
    CHECK(FH2_TR(&api, mod, key.c_str(), english.c_str()) == brazilian);
    CHECK(std::string(FH2_TR(&api, mod, "no.such.key", "Not translated")) == "Not translated");
    CHECK(std::string(FH2_TR(&api, mod, key.c_str(), "English it wasn't translated from")) == "English it wasn't translated from");
    char number[32];
    CHECK(api.format_number(1234.5, 1, number, sizeof number) > 0 && std::string(number) == "1.234,5");
    CHECK(std::string(FH2_TR_PLURAL(&api, mod, "test.plural", "{n} car", "{n} cars", 2)) == "{n} cars");
    set_game_language("cz");
    CHECK(std::string(api.language()) == "cs");
    CHECK(FH2_TR(&api, mod, key.c_str(), english.c_str()) == english);
    set_game_language("en");
    CHECK(std::string(api.language()) == "en");
    CHECK(api.format_number(1234.5, 1, number, sizeof number) > 0 && std::string(number) == "1,234.5");

    // The watcher records the game's language (the first it saw) for the next launch, in the mod's folder.
    std::string state;
    for (int i = 0; i < 30 && state.find("last=") == std::string::npos; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        state = read(folder + "/fh2-mods/platform-language.ini");
    }
    CHECK(state.find("\nlast=") != std::string::npos);

    // An unreadable manager: no language, no fault.
    std::memset(image.data() + kStringTables, 0, 8);
    CHECK(api.game_language(code, sizeof code) == 0);
    // Another exe (its image another size): no language read.
    set_game_language("br");
    eo_l10n_start(dir.c_str(), image.data(), 0x1000000, log_line);
    CHECK(api.game_language(code, sizeof code) == 0);
    std::printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 0;
}
