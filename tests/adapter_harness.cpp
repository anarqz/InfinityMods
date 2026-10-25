// MIT. Loads a built mod DLL with a fake Infinity runtime (no game, no hooks patched) and checks the
// adapter's contract. Usage: adapter_harness <dll> <settings-relative> <mode> [required-hook-count]
//   ready     one READY report within 20 s, after every required hook is made and enabled
//   badpath   the settings file is not where the recipe says: one FAILED report
//   sql0/sql1 InfinityModRegisterDatabaseV1 with the first startup-SQL switch at 0/1: 0/1+ statements
// The fake wait_for_code returns a zeroed, never-executed region; hooks only record their names.
#include "infinity_mod.h"
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {
std::atomic<int> reports{0}, last_state{-1}, hooks_made{0}, enables{0}, queued{0};
struct FakeHook { int id; } hook_store[256];

void report_state(InfinityMod*, uint32_t state) { reports++; last_state = static_cast<int>(state); }
void log_line(InfinityMod*, const char* line) { std::printf("  mod: %s\n", line); }
InfinityHook* make(const char* name) {
    const int n = hooks_made++;
    std::printf("  hook %d: %s\n", n, name ? name : "(unnamed)");
    return n < 256 ? reinterpret_cast<InfinityHook*>(&hook_store[n]) : nullptr;
}
InfinityHook* hook_function(InfinityMod*, const char* name, void*, void*, void** original) { *original = nullptr; return make(name); }
InfinityHook* hook_call(InfinityMod*, const char* name, void*, void*, void** original) { *original = nullptr; return make(name); }
InfinityHook* hook_pointer(InfinityMod*, const char* name, void**, void*, void** original) { *original = nullptr; return make(name); }
InfinityHook* patch_bytes(InfinityMod*, const char* name, void*, const void*, const void*, uint32_t) { return make(name); }
InfinityHook* hook_instruction(InfinityMod*, const char* name, void*, InfinityInstructionFn, void*) { return make(name); }
int enable_hooks(InfinityMod*, InfinityHook*) { enables++; return 1; }
int disable_hooks(InfinityMod*, InfinityHook*) { return 1; }
void* fake_image = nullptr;
void* wait_for_code(uint32_t) { return fake_image; }
int queue_sql(InfinityMod*, const char* sql) { queued++; std::printf("  sql: %s\n", sql); return 1; }

int fail(const char* why) { std::printf("FAIL %s\n", why); return 1; }
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 4) return fail("usage: adapter_harness <dll> <settings-relative> <mode> [required-hook-count]");
    const std::wstring mode = argv[3];
    const int required = argc > 4 ? _wtoi(argv[4]) : 0;
    const auto root = std::filesystem::temp_directory_path() / (L"infmods-harness-" + std::to_wstring(GetCurrentProcessId()));
    const auto folder = root / L"infinity-mods" / L"Mod";
    auto settings = folder / argv[2];
    if (mode == L"badpath") settings = folder / L"elsewhere.ini";
    std::filesystem::create_directories(settings.parent_path());
    { std::ofstream ini(settings); ini << "[Settings]\n" << (mode == L"sql0" ? "BackfireOnAllCars=0\n" : "BackfireOnAllCars=1\n"); }

    HMODULE dll = LoadLibraryW(argv[1]);
    if (!dll) return fail("LoadLibrary");
    static InfinityMod* const owner = reinterpret_cast<InfinityMod*>(&hook_store[255]);
    const std::wstring settings_text = settings.wstring(), azimuth_text = root.wstring();

    if (mode == L"sql0" || mode == L"sql1") {
        auto registerDb = reinterpret_cast<InfinityModRegisterDatabaseV1Fn>(reinterpret_cast<void*>(GetProcAddress(dll, "InfinityModRegisterDatabaseV1")));
        if (!registerDb) return fail("no InfinityModRegisterDatabaseV1");
        InfinityModDatabaseApiV1 db{sizeof db, INFINITY_MOD_ABI_V1, owner, settings_text.c_str(), queue_sql};
        if (registerDb(&db) != 0) return fail("InfinityModRegisterDatabaseV1 returned an error");
        if ((mode == L"sql0") != (queued == 0)) return fail("startup SQL does not follow the setting");
        std::printf("OK %ls: %d statement(s)\n", mode.c_str(), queued.load());
        return 0;
    }

    fake_image = VirtualAlloc(nullptr, 0x4000000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    auto init = reinterpret_cast<InfinityModInitializeV1Fn>(reinterpret_cast<void*>(GetProcAddress(dll, "InfinityModInitializeV1")));
    if (!init || !fake_image) return fail("no InfinityModInitializeV1");
    InfinityModApiV1 api{};
    api.size = sizeof api; api.abi_version = INFINITY_MOD_ABI_V1; api.mod = owner;
    api.azimuth_dir = azimuth_text.c_str(); api.settings_path = settings_text.c_str();
    api.report_state = report_state; api.log = log_line;
    api.hook_function = hook_function; api.hook_call = hook_call; api.hook_pointer = hook_pointer;
    api.enable_hooks = enable_hooks; api.disable_hooks = disable_hooks; api.wait_for_code = wait_for_code;
    api.patch_bytes = patch_bytes; api.hook_instruction = hook_instruction;
    const int started = init(&api);
    for (int i = 0; i < 200 && reports == 0; ++i) Sleep(100);
    Sleep(1000);  // a second report would show up here
    if (reports != 1) return fail("expected exactly one report");
    if (mode == L"badpath") {
        if (last_state != INFINITY_MOD_FAILED || started == 0) return fail("a misplaced settings file must fail");
        std::printf("OK badpath\n");
        return 0;
    }
    if (started != 0 || last_state != INFINITY_MOD_READY) return fail("not READY");
    if (hooks_made < required) return fail("fewer hooks than the recipe requires");
    std::printf("OK ready: %d hook(s), %d enable call(s)\n", hooks_made.load(), enables.load());
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 0);  // the mod's threads keep running; no unload
    return 0;
}
