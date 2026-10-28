/* MIT; see LICENSE. Runs an FH2_ModLoader (API 1) mod as an ExtraOptions plugin (OD-127).
   Linked into the mod's own DLL next to its unchanged sources: the mod keeps its FH2ModLoad and
   DllMain, this file exports FH2PluginInitialize, which ExtraOptions' plugin loader calls once, after
   DllMain has returned, on its loader worker. No Infinity mod runtime is involved. Freestanding (no
   CRT); every Win32 call it makes is in the upstream kernel32.def. */
#include "fh2_mod.h"
#include <stdint.h>

#define PATH_CAPACITY 1024

__declspec(dllexport) int FH2ModLoad(const FH2ModHost *host);

/* ExtraOptions' plugin ABI, version 1 (its fh2_plugin.h and tools/PLUGIN_API.md; unchanged from
   0.2.176 through 0.3.9): the 64-byte initializer context. Declared here from that header, so the
   mod needs nothing from ExtraOptions to build. */
typedef struct PluginContext {
    uint32_t size, api_version, adapter, game_image_size;
    void *game_image;
    HMODULE plugin_module;
    const wchar_t *plugins_directory;
    const wchar_t *plugin_path;
    const char *extra_options_version;
    void (*log_utf8)(const char *message);
} PluginContext;
_Static_assert(sizeof(PluginContext) == 64, "ExtraOptions plugin context: 64 bytes");
#define PLUGIN_API_VERSION 1u
#define PLUGIN_AZIMUTH 1u

static const PluginContext *g_context;
static volatile LONG g_started;
static FH2ModHost g_host;
static wchar_t g_exe[PATH_CAPACITY], g_data_dir[PATH_CAPACITY], g_mods_dir[PATH_CAPACITY], g_settings[PATH_CAPACITY];

/* One log_utf8 call per line (ExtraOptions appends each call as it comes), CRLF-terminated. */
static void log_line(const char *text) {
    static const char prefix[] = "FH2ModLoader adapter: ";
    char line[512];
    size_t used = 0;
    for (const char *c = prefix; *c; ++c) line[used++] = *c;
    for (const char *c = text; *c && used + 3 < sizeof line; ++c) line[used++] = *c;
    line[used++] = '\r';
    line[used++] = '\n';
    line[used] = 0;
    g_context->log_utf8(line);
}

static size_t text_length(const wchar_t *text) {
    size_t length = 0;
    while (text[length]) ++length;
    return length;
}

/* plugins_directory is <Azimuth>\mods\<mod>\plugins: data_dir and settings_dir become
   <Azimuth>\mods\<mod>\ and mods_dir <Azimuth>\mods\ (trailing separators, as FH2_ModLoader passes
   them). 0 when the DLL is not in a mod's plugins folder or a path is too long. */
static int derive_dirs(const wchar_t *plugins) {
    size_t length = plugins ? text_length(plugins) : 0;
    while (length && (plugins[length - 1] == L'\\' || plugins[length - 1] == L'/')) --length;
    size_t cut = length;
    while (cut && plugins[cut - 1] != L'\\' && plugins[cut - 1] != L'/') --cut;
    static const wchar_t name[] = L"plugins";
    if (length - cut != 7 || cut < 2) return 0;
    for (size_t i = 0; i < 7; ++i) {
        wchar_t c = plugins[cut + i];
        if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
        if (c != name[i]) return 0;
    }
    size_t mod = cut; /* includes the separator before plugins */
    size_t parent = mod - 1;
    while (parent && plugins[parent - 1] != L'\\' && plugins[parent - 1] != L'/') --parent;
    if (!parent || mod >= PATH_CAPACITY) return 0;
    for (size_t i = 0; i < mod; ++i) g_data_dir[i] = plugins[i] == L'/' ? L'\\' : plugins[i];
    g_data_dir[mod] = 0;
    for (size_t i = 0; i < parent; ++i) g_mods_dir[i] = g_data_dir[i];
    g_mods_dir[parent] = 0;
    return 1;
}

/* The launcher's form writes [Infinity] Enabled in <mod>\<DLL name>.ini; 0 keeps the mod off
   without disabling its folder in loadorder.txt. No INI: on. */
static int enabled_in_settings(void) {
    const wchar_t *path = g_context->plugin_path;
    size_t length = path ? text_length(path) : 0, name = length;
    while (name && path[name - 1] != L'\\' && path[name - 1] != L'/') --name;
    if (length < name + 5 || text_length(g_data_dir) + (length - name) >= PATH_CAPACITY) return 1;
    /* g_settings = g_data_dir + <DLL name without .dll> + ".ini" */
    size_t at = 0;
    for (size_t i = 0; g_data_dir[i]; ++i) g_settings[at++] = g_data_dir[i];
    for (size_t i = name; i < length - 4; ++i) g_settings[at++] = path[i];
    static const wchar_t ini[] = L".ini";
    for (size_t i = 0; i < 4; ++i) g_settings[at++] = ini[i];
    g_settings[at] = 0;
    wchar_t value[8];
    GetPrivateProfileStringW(L"Infinity", L"Enabled", L"1", value, 8, g_settings);
    return !(value[0] == L'0' && value[1] == 0);
}

/* An FH2ModGames export that leaves out Forza Horizon 2 means the mod is for another game. */
static int made_for_this_game(void) {
    const unsigned *games = (const unsigned *)(void *)GetProcAddress(g_context->plugin_module, "FH2ModGames");
    return !games || (*games & FH2_GAME_FORZA_HORIZON_2);
}

__declspec(dllexport) BOOL WINAPI FH2PluginInitialize(const PluginContext *context) {
    if (!context || context->size < sizeof *context || context->api_version != PLUGIN_API_VERSION
        || !context->game_image || !context->log_utf8 || !context->plugin_module) return FALSE;
    if (InterlockedExchange(&g_started, 1)) return FALSE;
    g_context = context;
    if (context->adapter != PLUGIN_AZIMUTH) { log_line("only Azimuth is supported."); return FALSE; }
    if (!derive_dirs(context->plugins_directory)) { log_line("the DLL is not in <mod>\\plugins\\."); return FALSE; }
    if (!made_for_this_game()) { log_line("the mod is not made for Forza Horizon 2."); return FALSE; }
    if (!enabled_in_settings()) { log_line("off in the mod's settings."); return TRUE; }
    /* Azimuth maps the game itself, so the image is usually not a loader module and has no path;
       a mod that needs game_exe gets an empty string and must check for it. */
    DWORD length = GetModuleFileNameW((HMODULE)context->game_image, g_exe, PATH_CAPACITY);
    if (!length || length >= PATH_CAPACITY) g_exe[0] = 0;
    g_host.size = sizeof g_host;
    g_host.api_version = FH2_MOD_API_VERSION;
    g_host.game_image = (unsigned char *)context->game_image;
    g_host.game_exe = g_exe;
    g_host.settings_dir = g_data_dir;
    g_host.mods_dir = g_mods_dir;
    g_host.data_dir = g_data_dir;
    g_host.game = FH2_GAME_FORZA_HORIZON_2;
    /* FH2ModLoad starts threads and returns; ExtraOptions calls this outside the loader lock, so
       no thread of our own is needed. */
    if (!FH2ModLoad(&g_host)) { log_line("FH2ModLoad refused to start."); return FALSE; }
    log_line("mod started.");
    return TRUE;
}
