/* MIT; see LICENSE. Runs an FH2_ModLoader (API 1) mod inside the Infinity mod runtime.
   Linked into the mod's own DLL next to its unchanged sources: the mod keeps its FH2ModLoad and
   DllMain, this file adds InfinityModInitializeV1. Freestanding (no CRT); every Win32 call it makes
   is in the upstream kernel32.def. */
#include "infinity_mod.h"
#include "fh2_mod.h"

#define PATH_CAPACITY 1024

__declspec(dllexport) int FH2ModLoad(const FH2ModHost *host);

static const InfinityModApiV1 *g_api;
static volatile LONG g_started;
static FH2ModHost g_host;
static wchar_t g_exe[PATH_CAPACITY], g_data_dir[PATH_CAPACITY], g_mods_dir[PATH_CAPACITY];

static void report(const char *line, uint32_t state) {
    g_api->log(g_api->mod, line);
    g_api->report_state(g_api->mod, state);
}

/* The directory of path, with its trailing separator; levels > 1 climbs further. 0 when too long. */
static int parent_dir(wchar_t *out, const wchar_t *path, int levels) {
    size_t length = 0;
    while (path[length]) ++length;
    if (length >= PATH_CAPACITY) return 0;
    while (levels-- > 0) {
        if (length && (path[length - 1] == L'\\' || path[length - 1] == L'/')) --length;
        while (length && path[length - 1] != L'\\' && path[length - 1] != L'/') --length;
        if (!length) return 0;
    }
    for (size_t i = 0; i < length; ++i) out[i] = path[i];
    out[length] = 0;
    return 1;
}

/* [Infinity] Enabled=0 in the mod's settings keeps it off without uninstalling. */
static int enabled_in_settings(void) {
    wchar_t value[8];
    GetPrivateProfileStringW(L"Infinity", L"Enabled", L"1", value, 8, g_api->settings_path);
    return !(value[0] == L'0' && value[1] == 0);
}

/* An FH2ModGames export that leaves out Forza Horizon 2 means the mod is for another game. */
static int made_for_this_game(void) {
    HMODULE self = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)(void *)&made_for_this_game, &self)) return 0;
    const unsigned *games = (const unsigned *)(void *)GetProcAddress(self, "FH2ModGames");
    return !games || (*games & FH2_GAME_FORZA_HORIZON_2);
}

static DWORD WINAPI start(LPVOID unused) {
    (void)unused;
    unsigned char *image = (unsigned char *)g_api->wait_for_code(60000);
    if (!image) {
        report("FH2ModLoader adapter: game code not found (timed out).", INFINITY_MOD_FAILED);
        return 0;
    }
    // Azimuth maps the game itself, so the image may not be a loader module; ABI 1 passes no game
    // folder. A mod that needs game_exe gets an empty string and must check for it.
    DWORD length = GetModuleFileNameW((HMODULE)image, g_exe, PATH_CAPACITY);
    if (!length || length >= PATH_CAPACITY) {
        g_exe[0] = 0;
        g_api->log(g_api->mod, "FH2ModLoader adapter: game executable path unavailable; game_exe is empty.");
    }
    // FH2_ModLoader's folders map to the mod's own Infinity folder: <azimuth>\infinity-mods\<Mod>\.
    g_host.size = sizeof g_host;
    g_host.api_version = FH2_MOD_API_VERSION;
    g_host.game_image = image;
    g_host.game_exe = g_exe;
    g_host.settings_dir = g_data_dir;
    g_host.mods_dir = g_mods_dir;
    g_host.data_dir = g_data_dir;
    g_host.game = FH2_GAME_FORZA_HORIZON_2;
    if (!FH2ModLoad(&g_host)) {
        report("FH2ModLoader adapter: FH2ModLoad refused to start.", INFINITY_MOD_FAILED);
        return 0;
    }
    report("FH2ModLoader adapter: mod started.", INFINITY_MOD_READY);
    return 0;
}

__declspec(dllexport) int InfinityModInitializeV1(const InfinityModApiV1 *api) {
    if (!api || api->size != sizeof *api || api->abi_version != INFINITY_MOD_ABI_V1 || !api->mod || !api->settings_path
        || !api->report_state || !api->log || !api->wait_for_code) return 1;
    if (InterlockedExchange(&g_started, 1)) return 2;
    g_api = api;
    if (!parent_dir(g_data_dir, api->settings_path, 1) || !parent_dir(g_mods_dir, api->settings_path, 2)) {
        report("FH2ModLoader adapter: settings path unusable.", INFINITY_MOD_FAILED);
        return 3;
    }
    if (!made_for_this_game()) {
        report("FH2ModLoader adapter: the mod is not made for Forza Horizon 2.", INFINITY_MOD_FAILED);
        return 3;
    }
    if (!enabled_in_settings()) {
        report("FH2ModLoader adapter: off in the mod's settings.", INFINITY_MOD_READY);
        return 0;
    }
    HANDLE thread = CreateThread(NULL, 0, start, NULL, 0, NULL);
    if (!thread) {
        report("FH2ModLoader adapter: could not start its thread.", INFINITY_MOD_FAILED);
        return 3;
    }
    CloseHandle(thread);
    return 0;
}
