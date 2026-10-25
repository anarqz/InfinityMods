// MIT; see LICENSE. Runs an FH2 Mod Platform (API 8) mod inside the Infinity mod runtime.
// Linked into the mod's DLL next to its unchanged sources: the mod keeps fh2mod_init, this file
// exports InfinityModInitializeV1 (game) and InfinityModRegisterDatabaseV1 (startup SQL). The
// recipe's adapter_config.h names the settings file, the required hooks and the startup SQL.
#include "infinity_mod.h"
#include "fh2mod.h"
#include "adapter_config.h"

#include <windows.h>
#include <atomic>
#include <cstring>
#include <cwchar>

extern "C" int fh2mod_init(const FH2ModApi* api);

namespace {
const InfinityModApiV1* host = nullptr;
std::atomic<bool> started{false}, reported{false};
FH2ModApi table{};
wchar_t azimuth_dir[MAX_PATH * 2];
// The mod's handle: one mod per DLL, so a fixed address stands for host->mod.
struct Owner { int unused; } owner;
FH2Mod* const self = reinterpret_cast<FH2Mod*>(&owner);

constexpr size_t required_count = sizeof(FH2MP_REQUIRED_HOOKS) / sizeof(FH2MP_REQUIRED_HOOKS[0]);
struct Made { FH2Hook* hook; int required; };
Made made[64];
std::atomic<unsigned> made_count{0};
bool enabled[required_count];
SRWLOCK lock = SRWLOCK_INIT;

void report(uint32_t state, const char* line) {
    if (reported.exchange(true)) return;
    host->log(host->mod, line);
    host->report_state(host->mod, state);
}

int required_index(const char* name) {
    for (size_t i = 0; name && i < required_count; ++i)
        if (!std::strcmp(name, FH2MP_REQUIRED_HOOKS[i])) return static_cast<int>(i);
    return -1;
}

FH2Hook* track(const char* name, InfinityHook* hook) {
    const int index = required_index(name);
    if (!hook) {
        if (index >= 0) report(INFINITY_MOD_FAILED, "FH2 Mod Platform adapter: a required hook could not be made.");
        return nullptr;
    }
    const unsigned slot = made_count.fetch_add(1);
    if (slot < 64) made[slot] = {reinterpret_cast<FH2Hook*>(hook), index};
    return reinterpret_cast<FH2Hook*>(hook);
}

// READY once every required hook is enabled; FAILED as soon as enabling a required one fails.
void note_enable(FH2Hook* hook, int ok) {
    AcquireSRWLockExclusive(&lock);
    const unsigned count = made_count.load() < 64 ? made_count.load() : 64;
    bool failed = false;
    for (unsigned i = 0; i < count; ++i) {
        if ((hook && made[i].hook != hook) || made[i].required < 0) continue;
        if (ok) enabled[made[i].required] = true; else failed = true;
    }
    bool all = true;
    for (size_t i = 0; i < required_count; ++i) all = all && enabled[i];
    ReleaseSRWLockExclusive(&lock);
    if (failed) report(INFINITY_MOD_FAILED, "FH2 Mod Platform adapter: a required hook could not be enabled.");
    else if (all) report(INFINITY_MOD_READY, "FH2 Mod Platform adapter: every required hook is enabled.");
}

InfinityMod* mod_of(FH2Mod*) { return host->mod; }
uint32_t bounded(uint32_t timeout) { return timeout > 60000 ? 60000 : timeout; }

FH2Mod* register_mod(const FH2ModInfo*) { return self; }
void log_line(FH2Mod*, const char* line) { if (line) host->log(host->mod, line); }
void ignored(const char* what) {
    char line[160] = "FH2 Mod Platform adapter: ";
    strncat_s(line, what, _TRUNCATE);
    host->log(host->mod, line);
}
void set_string(FH2Mod*, const char*, uint16_t, const wchar_t*) { ignored("string tables are not supported; ignored."); }
void replace_string(FH2Mod*, const char*, const wchar_t*, const wchar_t*) { ignored("string tables are not supported; ignored."); }
void clear_strings(FH2Mod*, const char*) {}
void apply_strings(FH2Mod*) {}
void set_notice(FH2Mod*, const char*) {}
void* game_base() { return host->wait_for_code(0); }
void* wait_for_game(uint32_t timeout) { return host->wait_for_code(bounded(timeout)); }
int set_file(FH2Mod*, const char*, const void*, size_t) { ignored("set_file is not supported: map the file in the manifest."); return 0; }
void clear_file(FH2Mod*, const char*) {}
void add_sql(FH2Mod*, const char*) { ignored("add_sql in the game is ignored: startup SQL comes from the recipe."); }
void clear_sql(FH2Mod*) {}
void apply_database(FH2Mod*) {}
void publish_interface(FH2Mod*, const char*, uint32_t, const void*) {}
const void* get_interface(const char*, uint32_t) { return nullptr; }
int wait_for_mods(uint32_t) { return 1; }
const char* mod_version(const char*) { return nullptr; }
FH2Hook* hook_function(FH2Mod* m, const char* name, void* target, void* detour, void** original) {
    return track(name, host->hook_function(mod_of(m), name, target, detour, original));
}
FH2Hook* hook_call(FH2Mod* m, const char* name, void* site, void* detour, void** original) {
    return track(name, host->hook_call(mod_of(m), name, site, detour, original));
}
FH2Hook* hook_pointer(FH2Mod* m, const char* name, void** slot, void* detour, void** original) {
    return track(name, host->hook_pointer(mod_of(m), name, slot, detour, original));
}
int enable_hooks(FH2Mod* m, FH2Hook* hook) {
    const int ok = host->enable_hooks(mod_of(m), reinterpret_cast<InfinityHook*>(hook));
    note_enable(hook, ok);
    return ok;
}
int disable_hooks(FH2Mod* m, FH2Hook* hook) { return host->disable_hooks(mod_of(m), reinterpret_cast<InfinityHook*>(hook)); }
void* wait_for_code(uint32_t timeout) { return host->wait_for_code(bounded(timeout)); }
// Settings belong to the launcher's form; rows for the game's Mods page are accepted and never shown.
int add_setting(FH2Mod*, const FH2Setting*) { return 1; }
int get_setting(FH2Mod*, const char*) { return 0; }
void set_setting(FH2Mod*, const char*, int) {}
float get_slider(FH2Mod*, const char*) { return 0.0f; }
void set_slider(FH2Mod*, const char*, float) {}
FH2Hook* patch_bytes(FH2Mod* m, const char* name, void* at, const void* expect, const void* bytes, uint32_t size) {
    return track(name, host->patch_bytes(mod_of(m), name, at, expect, bytes, size));
}
FH2Hook* hook_instruction(FH2Mod* m, const char* name, void* at, FH2InstructionFn fn, void* user) {
    // FH2Context and InfinityContext have the same layout.
    return track(name, host->hook_instruction(mod_of(m), name, at, reinterpret_cast<InfinityInstructionFn>(fn), user));
}
int add_page(FH2Mod*, const FH2Page*) { return 0; }
int set_page_rows(FH2Mod*, const char*, const FH2PageRow*, uint32_t) { return 0; }
int set_page_xaml(FH2Mod*, const char*, const char*, const char*, const char*) { return 0; }
int get_page_slot(FH2Mod*, const char*, int) { return 0; }
int set_page_slot(FH2Mod*, const char*, int, int) { return 0; }
int focus_page_element(FH2Mod*, const char*, const char*) { return 0; }
int set_page_tile_text(FH2Mod*, const char*, const char*) { return 0; }
int set_page_tile_colors(FH2Mod*, const char*, uint32_t, uint32_t) { return 0; }

// The mod builds <azimuth_dir>\<FH2MP_SETTINGS_RELATIVE>; azimuth_dir is what makes that the
// launcher's settings file, so everything the mod reads stays in its own Infinity folder.
bool derive_azimuth_dir(const wchar_t* settings) {
    const size_t total = std::wcslen(settings), suffix = std::wcslen(FH2MP_SETTINGS_RELATIVE);
    if (total <= suffix + 1 || total - suffix >= sizeof azimuth_dir / sizeof azimuth_dir[0]) return false;
    if (_wcsicmp(settings + total - suffix, FH2MP_SETTINGS_RELATIVE) || settings[total - suffix - 1] != L'\\') return false;
    wmemcpy(azimuth_dir, settings, total - suffix - 1);
    azimuth_dir[total - suffix - 1] = 0;
    return true;
}

DWORD WINAPI watchdog(LPVOID) {
    Sleep(FH2MP_READY_TIMEOUT_MS);
    report(INFINITY_MOD_FAILED, "FH2 Mod Platform adapter: required hooks not enabled in time.");
    return 0;
}

void fill_table() {
    table.size = sizeof table;
    table.api_version = FH2MOD_API_VERSION;
    table.platform_version = FH2MP_PLATFORM_VERSION;
    table.azimuth_dir = azimuth_dir;
    table.game_dir = L"";
    table.register_mod = register_mod; table.log = log_line;
    table.set_string = set_string; table.replace_string = replace_string; table.clear_strings = clear_strings;
    table.apply_strings = apply_strings; table.set_notice = set_notice;
    table.game_base = game_base; table.wait_for_game = wait_for_game;
    table.set_file = set_file; table.clear_file = clear_file;
    table.add_sql = add_sql; table.clear_sql = clear_sql; table.apply_database = apply_database;
    table.publish_interface = publish_interface; table.get_interface = get_interface;
    table.wait_for_mods = wait_for_mods; table.mod_version = mod_version;
    table.hook_function = hook_function; table.hook_call = hook_call; table.hook_pointer = hook_pointer;
    table.enable_hooks = enable_hooks; table.disable_hooks = disable_hooks; table.wait_for_code = wait_for_code;
    table.add_setting = add_setting; table.get_setting = get_setting; table.set_setting = set_setting;
    table.get_slider = get_slider; table.set_slider = set_slider;
    table.patch_bytes = patch_bytes; table.hook_instruction = hook_instruction;
    table.add_page = add_page; table.set_page_rows = set_page_rows; table.set_page_xaml = set_page_xaml;
    table.get_page_slot = get_page_slot; table.set_page_slot = set_page_slot;
    table.focus_page_element = focus_page_element; table.set_page_tile_text = set_page_tile_text;
    table.set_page_tile_colors = set_page_tile_colors;
}
}

extern "C" __declspec(dllexport) int InfinityModInitializeV1(const InfinityModApiV1* api) {
    if (!api || api->size != sizeof *api || api->abi_version != INFINITY_MOD_ABI_V1 || !api->mod || !api->azimuth_dir
        || !api->settings_path || !api->report_state || !api->log || !api->wait_for_code || !api->hook_function
        || !api->hook_call || !api->hook_pointer || !api->enable_hooks || !api->disable_hooks || !api->patch_bytes
        || !api->hook_instruction) return 1;
    if (started.exchange(true)) return 2;
    host = api;
    if (!derive_azimuth_dir(api->settings_path)) {
        report(INFINITY_MOD_FAILED, "FH2 Mod Platform adapter: the settings file is not where the recipe says.");
        return 3;
    }
    fill_table();
    HANDLE timer = CreateThread(nullptr, 0, watchdog, nullptr, 0, nullptr);
    if (!timer) { report(INFINITY_MOD_FAILED, "FH2 Mod Platform adapter: could not start its watchdog."); return 3; }
    CloseHandle(timer);
    if (fh2mod_init(&table) != 0) {
        report(INFINITY_MOD_FAILED, "FH2 Mod Platform adapter: fh2mod_init refused to start.");
        return 3;
    }
    if (required_count == 1 && !FH2MP_REQUIRED_HOOKS[0][0])
        report(INFINITY_MOD_READY, "FH2 Mod Platform adapter: started (no required hooks).");
    return 0;
}

// Runs in Infinity's preparation process, before the game: the recipe's startup SQL, never fh2mod_init.
extern "C" __declspec(dllexport) int InfinityModRegisterDatabaseV1(const InfinityModDatabaseApiV1* api) {
    if (!api || api->size != sizeof *api || api->abi_version != 1 || !api->mod || !api->settings_path || !api->queue_sql) return 1;
    for (const auto& rule : FH2MP_STARTUP_SQL) {
        if (!rule.sql) continue;
        if (GetPrivateProfileIntW(rule.section, rule.key, rule.fallback, api->settings_path) == 0) continue;
        if (!api->queue_sql(api->mod, rule.sql)) return 1;
    }
    return 0;
}
