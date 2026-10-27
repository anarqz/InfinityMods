// MIT; see LICENSE. Runs an FH2 Mod Platform (API 8) mod as an ExtraOptions plugin (OD-126).
// Linked into the mod's DLL next to its unchanged sources and MinHook: the mod keeps fh2mod_init,
// this file exports FH2PluginInitialize (ExtraOptions' loader calls it once, after DllMain, on its
// worker) and FH2PluginDatabaseUpdate (the merged in-memory game database, on the game thread),
// and gives fh2mod_init an FH2ModApi table it implements itself. No Infinity runtime is involved.
#include "fh2mod.h"
#include "MinHook.h"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cwchar>

extern "C" int fh2mod_init(const FH2ModApi* api);

namespace {
// ExtraOptions' plugin ABI, version 1 (its tools/PLUGIN_API.md): the 64-byte initializer context
// and the database frame. Declared here from that documentation; the layouts never change.
struct PluginContext {
    uint32_t size, api_version, adapter, game_image_size;
    void* game_image;
    HMODULE plugin_module;
    const wchar_t* plugins_directory;
    const wchar_t* plugin_path;
    const char* extra_options_version;
    void (*log_utf8)(const char* message);
};
static_assert(sizeof(PluginContext) == 64, "ExtraOptions plugin context: 64 bytes");
struct DatabaseFrame {
    uint32_t size, api_version;
    void* database;
    int (*prepare)(void*, const char*, int, void**, const char**);
    int (*step)(void*);
    int (*finalize)(void*);
    int64_t (*column_int64)(void*, int);
};
constexpr uint32_t kAzimuth = 1;
constexpr int kSqliteOk = 0, kSqliteRow = 100, kSqliteDone = 101;

const PluginContext* host = nullptr;
std::atomic<bool> started{false};
FH2ModApi table{};
wchar_t mod_dir[MAX_PATH * 2];
struct Owner { int unused; } owner;
FH2Mod* const self = reinterpret_cast<FH2Mod*>(&owner);

// One log_utf8 call per line: ExtraOptions appends each call on its own, so the mod's threads never interleave a line.
void log_text(const char* a, const char* b = nullptr) {
    if (!host || !host->log_utf8) return;
    char line[1024];
    size_t used = 0;
    const char* const parts[] = {a, b, "\r\n"};
    for (const char* part : parts) {
        for (const char* c = part; c && *c && used + 3 < sizeof line; ++c) line[used++] = *c;
    }
    if (used < 2 || line[used - 1] != '\n') { line[used++] = '\r'; line[used++] = '\n'; }
    line[used] = 0;
    host->log_utf8(line);
}
void adapter_log(const char* line) { log_text("FH2 Mod Platform adapter: ", line); }

// Hooks: a function detour through MinHook, or a rel32 call site redirected through a jump stub
// allocated within +/-2 GB of the site. Created disabled; enable_hooks turns them on.
enum class Kind { Function, Call };
struct Hook {
    Kind kind;
    void* target;      // Function: the entry; Call: the E8 instruction
    void* stub;        // Call: the jump stub
    int32_t original_rel, detour_rel;
    bool enabled;
};
Hook hooks[64];
std::atomic<unsigned> hook_count{0};
SRWLOCK lock = SRWLOCK_INIT;
bool minhook_ready = false;

void* near_alloc(void* site, size_t size) {
    SYSTEM_INFO info; GetSystemInfo(&info);
    const uintptr_t granule = info.dwAllocationGranularity, center = reinterpret_cast<uintptr_t>(site);
    const uintptr_t low = center > 0x7ff00000u ? center - 0x7ff00000u : granule, high = center + 0x7ff00000u;
    for (uintptr_t at = (low + granule - 1) & ~(granule - 1); at < high; at += granule) {
        MEMORY_BASIC_INFORMATION region;
        if (!VirtualQuery(reinterpret_cast<void*>(at), &region, sizeof region)) break;
        if (region.State != MEM_FREE) {
            at = ((reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize + granule - 1) & ~(granule - 1)) - granule;
            continue;
        }
        if (void* p = VirtualAlloc(reinterpret_cast<void*>(at), size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)) return p;
    }
    return nullptr;
}

bool write_rel(void* site, int32_t rel) {
    DWORD old = 0;
    auto* at = static_cast<uint8_t*>(site) + 1;
    if (!VirtualProtect(at, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(at, &rel, 4);
    VirtualProtect(at, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    return true;
}

Hook* add(Kind kind, void* target) {
    const unsigned slot = hook_count.fetch_add(1);
    if (slot >= 64) { adapter_log("more than 64 hooks; refused."); return nullptr; }
    hooks[slot] = {kind, target, nullptr, 0, 0, false};
    return &hooks[slot];
}

bool set_one(Hook& hook, bool on) {
    if (hook.enabled == on) return true;
    bool ok = hook.kind == Kind::Function
        ? (on ? MH_EnableHook(hook.target) : MH_DisableHook(hook.target)) == MH_OK
        : write_rel(hook.target, on ? hook.detour_rel : hook.original_rel);
    if (ok) hook.enabled = on;
    return ok;
}

int set_hooks(FH2Hook* which, bool on) {
    AcquireSRWLockExclusive(&lock);
    const unsigned count = hook_count.load() < 64 ? hook_count.load() : 64;
    bool ok = true;
    for (unsigned i = 0; i < count; ++i)
        if (!which || reinterpret_cast<FH2Hook*>(&hooks[i]) == which) ok = set_one(hooks[i], on) && ok;
    ReleaseSRWLockExclusive(&lock);
    if (!ok) adapter_log(on ? "a hook could not be enabled." : "a hook could not be disabled.");
    return ok ? 1 : 0;
}

FH2Hook* hook_function(FH2Mod*, const char* name, void* target, void* detour, void** original) {
    if (!minhook_ready || !target || !detour || !original) return nullptr;
    if (MH_CreateHook(target, detour, original) != MH_OK) { log_text("FH2 Mod Platform adapter: hook not made: ", name ? name : "?"); return nullptr; }
    return reinterpret_cast<FH2Hook*>(add(Kind::Function, target));
}

FH2Hook* hook_call(FH2Mod*, const char* name, void* site, void* detour, void** original) {
    if (!site || !detour || !original || *static_cast<uint8_t*>(site) != 0xE8) {
        log_text("FH2 Mod Platform adapter: not a call site: ", name ? name : "?");
        return nullptr;
    }
    int32_t rel; std::memcpy(&rel, static_cast<uint8_t*>(site) + 1, 4);
    auto* next = static_cast<uint8_t*>(site) + 5;
    auto* stub = static_cast<uint8_t*>(near_alloc(site, 16));
    if (!stub) { log_text("FH2 Mod Platform adapter: no stub memory near: ", name ? name : "?"); return nullptr; }
    static const uint8_t jump[6] = {0xFF, 0x25, 0, 0, 0, 0}; // jmp [rip+0]
    std::memcpy(stub, jump, 6);
    std::memcpy(stub + 6, &detour, 8);
    FlushInstructionCache(GetCurrentProcess(), stub, 14);
    *original = next + rel;
    Hook* hook = add(Kind::Call, site);
    if (!hook) return nullptr;
    hook->stub = stub; hook->original_rel = rel;
    hook->detour_rel = static_cast<int32_t>(stub - next);
    return reinterpret_cast<FH2Hook*>(hook);
}

FH2Hook* unsupported_hook(const char* what) { adapter_log(what); return nullptr; }
FH2Hook* hook_pointer(FH2Mod*, const char*, void**, void*, void**) { return unsupported_hook("hook_pointer is not supported."); }
FH2Hook* patch_bytes(FH2Mod*, const char*, void*, const void*, const void*, uint32_t) { return unsupported_hook("patch_bytes is not supported."); }
FH2Hook* hook_instruction(FH2Mod*, const char*, void*, FH2InstructionFn, void*) { return unsupported_hook("hook_instruction is not supported."); }
int enable_hooks(FH2Mod*, FH2Hook* hook) { return set_hooks(hook, true); }
int disable_hooks(FH2Mod*, FH2Hook* hook) { return set_hooks(hook, false); }

// The mod's SQL (its add_sql calls in fh2mod_init, under its own INI conditions) runs on ExtraOptions'
// merged in-memory database; the mod folder's database.json makes ExtraOptions build that merge.
char* sql[16];
std::atomic<unsigned> sql_count{0};
void add_sql(FH2Mod*, const char* statement) {
    if (!statement) return;
    const size_t length = std::strlen(statement);
    const unsigned slot = sql_count.load();
    if (slot >= 16 || length > 4096) { adapter_log("add_sql: too many or too long; ignored."); return; }
    char* copy = static_cast<char*>(HeapAlloc(GetProcessHeap(), 0, length + 1));
    if (!copy) return;
    std::memcpy(copy, statement, length + 1);
    sql[slot] = copy;
    sql_count.store(slot + 1);
}
void clear_sql(FH2Mod*) { sql_count.store(0); }
void apply_database(FH2Mod*) {}

bool exec(const DatabaseFrame* f, const char* statement) {
    void* stmt = nullptr;
    if (f->prepare(f->database, statement, -1, &stmt, nullptr) != kSqliteOk || !stmt) return false;
    int rc;
    while ((rc = f->step(stmt)) == kSqliteRow) {}
    f->finalize(stmt);
    return rc == kSqliteDone;
}

FH2Mod* register_mod(const FH2ModInfo*) { return self; }
void log_line(FH2Mod*, const char* line) { if (line) log_text(line); }
void ignored(const char* what) { adapter_log(what); }
void set_string(FH2Mod*, const char*, uint16_t, const wchar_t*) { ignored("string tables are not supported; ignored."); }
void replace_string(FH2Mod*, const char*, const wchar_t*, const wchar_t*) { ignored("string tables are not supported; ignored."); }
void clear_strings(FH2Mod*, const char*) {}
void apply_strings(FH2Mod*) {}
void set_notice(FH2Mod*, const char*) {}
void* game_base() { return host ? host->game_image : nullptr; }
void* wait_for_game(uint32_t) { return game_base(); }
void* wait_for_code(uint32_t) { return game_base(); }
int set_file(FH2Mod*, const char*, const void*, size_t) { ignored("set_file is not supported: ship the file in media\\."); return 0; }
void clear_file(FH2Mod*, const char*) {}
void publish_interface(FH2Mod*, const char*, uint32_t, const void*) {}
const void* get_interface(const char*, uint32_t) { return nullptr; }
int wait_for_mods(uint32_t) { return 1; }
const char* mod_version(const char*) { return nullptr; }
// Settings belong to the launcher's form (the INI in the mod folder); rows for the game's Mods page are never shown.
int add_setting(FH2Mod*, const FH2Setting*) { return 1; }
int get_setting(FH2Mod*, const char*) { return 0; }
void set_setting(FH2Mod*, const char*, int) {}
float get_slider(FH2Mod*, const char*) { return 0.0f; }
void set_slider(FH2Mod*, const char*, float) {}
int add_page(FH2Mod*, const FH2Page*) { return 0; }
int set_page_rows(FH2Mod*, const char*, const FH2PageRow*, uint32_t) { return 0; }
int set_page_xaml(FH2Mod*, const char*, const char*, const char*, const char*) { return 0; }
int get_page_slot(FH2Mod*, const char*, int) { return 0; }
int set_page_slot(FH2Mod*, const char*, int, int) { return 0; }
int focus_page_element(FH2Mod*, const char*, const char*) { return 0; }
int set_page_tile_text(FH2Mod*, const char*, const char*) { return 0; }
int set_page_tile_colors(FH2Mod*, const char*, uint32_t, uint32_t) { return 0; }

// azimuth_dir is the mod's own folder (<Azimuth>\mods\<mod>, the parent of plugins\): the paths the mod
// builds from it (its INI, its sounds) stay inside that folder.
bool derive_mod_dir(const wchar_t* plugins) {
    size_t length = plugins ? std::wcslen(plugins) : 0;
    while (length && (plugins[length - 1] == L'\\' || plugins[length - 1] == L'/')) --length;
    size_t cut = length;
    while (cut && plugins[cut - 1] != L'\\' && plugins[cut - 1] != L'/') --cut;
    if (cut < 2 || _wcsnicmp(plugins + cut, L"plugins", length - cut) || length - cut != 7) return false;
    if (cut - 1 >= sizeof mod_dir / sizeof mod_dir[0]) return false;
    wmemcpy(mod_dir, plugins, cut - 1);
    mod_dir[cut - 1] = 0;
    return true;
}

void fill_table() {
    table.size = sizeof table;
    table.api_version = FH2MOD_API_VERSION;
    table.platform_version = "2.7.1";
    table.azimuth_dir = mod_dir;
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

extern "C" __declspec(dllexport) BOOL WINAPI FH2PluginInitialize(const PluginContext* context) {
    if (!context || context->size < 64 || context->api_version != 1 || !context->game_image || !context->log_utf8) return FALSE;
    if (started.exchange(true)) return FALSE;
    host = context;
    if (context->adapter != kAzimuth) { adapter_log("only Azimuth is supported."); return FALSE; }
    if (!derive_mod_dir(context->plugins_directory)) { adapter_log("the DLL is not in <mod>\\plugins\\."); return FALSE; }
    minhook_ready = MH_Initialize() == MH_OK;
    if (!minhook_ready) { adapter_log("MinHook could not start."); return FALSE; }
    fill_table();
    if (fh2mod_init(&table) != 0) { adapter_log("fh2mod_init refused to start."); return FALSE; }
    adapter_log("started.");
    return TRUE;
}

// ExtraOptions calls this once per merge, on the game thread, before the game reads its database.
extern "C" __declspec(dllexport) BOOL WINAPI FH2PluginDatabaseUpdate(const DatabaseFrame* frame) {
    if (!frame || frame->size < sizeof *frame || !frame->database || !frame->prepare || !frame->step || !frame->finalize) return FALSE;
    const unsigned count = sql_count.load();
    if (!count) return TRUE;
    if (!exec(frame, "SAVEPOINT fh2mp_adapter;")) return FALSE;
    for (unsigned i = 0; i < count; ++i) {
        if (!exec(frame, sql[i])) {
            exec(frame, "ROLLBACK TO fh2mp_adapter;");
            exec(frame, "RELEASE fh2mp_adapter;");
            adapter_log("the mod's SQL failed; the database is unchanged.");
            return FALSE;
        }
    }
    if (!exec(frame, "RELEASE fh2mp_adapter;")) return FALSE;
    adapter_log("the mod's SQL is applied to the merged database.");
    return TRUE;
}
