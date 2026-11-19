/* EasyRadio: an ExtraOptions plugin (ABI 1, FH2PluginInitialize; loaded by ExtraOptions' PluginLoader from
   mods/Forza Horizon 2/easyradio/plugins/) that makes official FH2 stations play folders of MP3s.
   FH2 exe sha256 3ec04c61...7f0a3 (Azimuth 1.0.4) only.

   At game start, on ExtraOptions' loader worker:
     1. checks the game code it touches (nothing is patched on any difference);
     2. hooks RadioSystem::Load (vtable 0x142e76818 slot +0x08 = 0x14100fd20), which reads
        Game:\Media\Audio\RadioInfo_<lang>.xml once, at audio init (AudioManager init 0x14100a4c0 stores the
        RadioSystem in 0x14365d410, then calls Load). The hook holds Load until step 3 is done, so the game reads
        the generated file, never a half-written or a stale one;
     3. generates everything (er_gen.c): reads easyradio.ini, scans the radio folders (ID3 tags, MP3 durations, a
        cache keyed by size and mtime), shuffles each station, writes RadioInfo_<lang>.xml for the 10 languages and
        the HUD icons into plugins/generated/, its private media root (FH2PluginMediaDirectory: ExtraOptions serves
        those files as this mod's media, looked up per read);
     4. installs the playback hooks (live-proven): getSoundInfo answers EASYRADIO_ keys
        with the MP3's path and our own FMOD file callbacks (CreateFileW), getSubSound returns a plain stream
        itself, and the skip (F6 / a left-stick tap) moves the station clock and re-plays through the game's own
        RadioStreamFmod::Seek.
   If RadioSystem::Load ran before this plugin started, nothing is published and the radio stays stock for this
   start (logged). Every other problem is logged and skipped; with nothing valid the radio stays stock.

   install_hook, the thread freeze and the pad read come from FH Mods Collection (spotify-radio/spotify_bridge.c,
   infinity/eo_plugin.c), GPL-3.0-or-later; see NOTICE. Freestanding C, kernel32/user32 only, no CRT. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdarg.h>
#include <stdint.h>

#include "er_gen.h"

#define EASYRADIO_VERSION "1.2.0"

/* ---- ExtraOptions plugin ABI 1 (fh2_plugin.h) ---- */
#define FH2_PLUGIN_API_VERSION 1u
enum { FH2_PLUGIN_AZIMUTH = 1 };
typedef struct FH2PluginContext {
    uint32_t size, api_version, adapter, game_image_size;
    void *game_image;
    HMODULE plugin_module;
    const wchar_t *plugins_directory;
    const wchar_t *plugin_path;
    const char *extra_options_version;
    void (*log_utf8)(const char *message);
} FH2PluginContext;

/* ---- the game ---- */
#define GAME_IMAGE_SIZE 0x3a18000u
#define RVA_GET_SOUND_INFO 0x1a98920u
#define RVA_GET_SUB_SOUND 0x199f510u
#define RVA_CREATE_SOUND 0x1997740u
#define RVA_GET_OPEN_STATE 0x199f2e0u
#define RVA_GET_TIMELINE_POSITION 0x1a98b10u
#define RVA_STATION_UPDATE 0x1024fc0u
#define RVA_STREAM_VTABLE 0x2e76220u /* RadioStreamFmod */
#define RVA_STREAM_SEEK 0x1021150u   /* RadioStreamFmod vtable +0x58: (stream, offset ms) re-plays the current entry at offset */
#define RVA_RADIO_SYSTEM_LOAD 0x100fd20u
#define RVA_RADIO_SYSTEM_LOAD_SLOT 0x2e76820u /* RadioSystem vtable 0x142e76818 +0x08 */
#define RVA_RADIO_SYSTEM 0x365d410u           /* the RadioSystem, stored right before its Load call */
#define RVA_RADIO_LOAD_CALL 0x100a70eu        /* AudioManager init: store the RadioSystem, then call its Load */
#define STATION_SUBSTATE 0x100
#define STATION_STATE 0x104
#define STATION_NUMBER 0x108
#define STATION_TRACK_CLOCK 0x20 /* track player (+0x08) elapsed ms, float */
#define STATION_TRACK_ACTIVE 0x24
#define RVA_UPDATE_POSITION_RETURN 0x10251ebu /* RadioStreamFmod::Update, after its getTimelinePosition call */
#define STREAM_POSITION_OFFSET 0x2c
#define STREAM_SOUND_OFFSET 0x08
#define RVA_GET_NUM_SUB_SOUNDS 0x199ef60u
#define RVA_GET_LENGTH 0x199e5a0u
/* pad state, as spotify_bridge.c game_buttons (the same read as Extra Options' autodrive) */
#define RVA_INPUT_MANAGER 0x365d7c8u
#define RVA_INPUT_MANAGER_VTABLE 0x2e7a2e8u

/* build.py reads these lines (name, rva comment) and compares them with the exe on disk. */
static const unsigned char prologue_get_sound_info[] = {0x40, 0x53, 0x55, 0x56, 0x41, 0x56, 0x48, 0x81, 0xec, 0x48, 0x01, 0x00, 0x00}; // rva 0x1a98920
static const unsigned char prologue_get_sub_sound[] = {0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x56, 0x48, 0x81, 0xec, 0x60, 0x01, 0x00, 0x00}; // rva 0x199f510
static const unsigned char prologue_create_sound[] = {0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xec, 0x68, 0x01, 0x00, 0x00}; // rva 0x1997740
static const unsigned char prologue_get_open_state[] = {0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xec, 0x68, 0x01, 0x00, 0x00}; // rva 0x199f2e0
static const unsigned char prologue_get_timeline_position[] = {0x48, 0x89, 0x5c, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x81, 0xec, 0x40, 0x01, 0x00, 0x00}; // rva 0x1a98b10
static const unsigned char prologue_station_update[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18}; // rva 0x1024fc0
/* called, not hooked: checked so a wrong build never calls into other code */
static const unsigned char prologue_stream_seek[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b, 0xd9, 0x48, 0x8b, 0x49, 0x10}; // rva 0x1021150
/* the call site the skip hook trusts: call getTimelinePosition (e8 rel32 -> 0x141a98b10) at 0x1410251e6 */
static const unsigned char prologue_update_position_call[] = {0x48, 0x8d, 0x53, 0x2c, 0xe8, 0x25, 0x39, 0xa7, 0x00}; // rva 0x10251e2
/* RadioSystem::Load (reached through its vtable slot, which is what we replace) */
static const unsigned char prologue_radio_load[] = {0x48, 0x8b, 0xc4, 0x55, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0x68, 0xa1}; // rva 0x100fd20
/* AudioManager init: mov [0x14365d410], rcx; cmp; cmove; mov rax,[rcx]; lea r8,[rbx+0x428]; mov rdx,rsi; call [rax+8] */
static const unsigned char prologue_radio_load_call[] = {0x48, 0x89, 0x0d, 0xfb, 0x2c, 0x65, 0x02, 0x80, 0xbb, 0xf8, 0x04, 0x00, 0x00, 0x00, 0x49, 0x0f, 0x44, 0xf6, 0x48, 0x8b, 0x01, 0x4c, 0x8d, 0x83, 0x28, 0x04, 0x00, 0x00, 0x48, 0x8b, 0xd6, 0xff, 0x50, 0x08}; // rva 0x100a70e

/* FMOD_STUDIO_SOUND_INFO as RadioStreamFmod::Play 0x141018840 lays it out: name_or_data +0, mode +8,
   exinfo +0x10 (FMOD_CREATESOUNDEXINFO, cbsize 0xd8), subsoundindex +0xe8. */
#define INFO_NAME 0x00
#define INFO_MODE 0x08
#define INFO_EXINFO 0x10
#define EXINFO_SIZE 0xd8
#define INFO_SUBSOUND 0xe8
#define INFO_BYTES 0xec
#define FMOD_ACCURATETIME 0x4000u
#define FMOD_TIMEUNIT_MS 1u

typedef int (*GetSoundInfoFn)(void *studio, const char *key, unsigned char *info);
typedef int (*GetSubSoundFn)(void *sound, int index, void **out);
typedef int (*CreateSoundFn)(void *system, const char *name, unsigned mode, void *exinfo, void **out);
typedef int (*GetOpenStateFn)(void *sound, int *state, unsigned *percent, unsigned char *starving, unsigned char *busy);
typedef int (*GetTimelinePositionFn)(void *instance, int *position);
typedef void (*StationUpdateFn)(void *station, void *context);
typedef void (*StreamSeekFn)(void *stream, int offset);
typedef int (*GetNumSubSoundsFn)(void *sound, int *count);
typedef int (*GetLengthFn)(void *sound, unsigned *length, unsigned unit);
typedef void (*RadioLoadFn)(void *radio_system, void *banks, void *language);

static GetSoundInfoFn original_get_sound_info;
static GetSubSoundFn original_get_sub_sound;
static CreateSoundFn original_create_sound;
static GetOpenStateFn original_get_open_state;
static GetTimelinePositionFn original_get_timeline_position;
static StationUpdateFn original_station_update;
static RadioLoadFn original_radio_load;
static GetNumSubSoundsFn get_num_sub_sounds;
static GetLengthFn get_length;
static unsigned char *game_base;

/* ---- freestanding helpers (the compiler may emit calls to these) ---- */
void *memset(void *d, int c, size_t n) {
    unsigned char *p = d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}
void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *p = d;
    const unsigned char *q = s;
    while (n--) *p++ = *q++;
    return d;
}
void *memmove(void *d, const void *s, size_t n) {
    unsigned char *p = d;
    const unsigned char *q = s;
    if (p < q) {
        while (n--) *p++ = *q++;
    } else {
        p += n, q += n;
        while (n--) *--p = *--q;
    }
    return d;
}
int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *p = a, *q = b;
    for (size_t i = 0; i < n; ++i)
        if (p[i] != q[i]) return p[i] - q[i];
    return 0;
}
size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) ++n;
    return n;
}
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) ++a, ++b;
    return (unsigned char)*a - (unsigned char)*b;
}
int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}
int abs(int v) {
    return v < 0 ? -v : v;
}
static int memory_equal(const void *a, const void *b, size_t n) {
    return memcmp(a, b, n) == 0;
}
/* reads that never fault (the pad state lives in game objects that may not exist yet) */
static int memory_read(const void *address, void *out, size_t size) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), address, out, size, &got) && got == size;
}

/* ---- log: <mod>\EasyRadio.log ---- */
static HANDLE log_file = INVALID_HANDLE_VALUE;
static SRWLOCK log_lock = SRWLOCK_INIT;

void er_log_line(const char *line) {
    char stamp[32];
    ErBuf b = {0};
    ULONGLONG ms = GetTickCount64();
    er_buf_format(&b, "[%llu.", (unsigned long long)(ms / 1000));
    unsigned rest = (unsigned)(ms % 1000);
    stamp[0] = (char)('0' + rest / 100), stamp[1] = (char)('0' + rest / 10 % 10), stamp[2] = (char)('0' + rest % 10), stamp[3] = 0;
    er_buf_str(&b, stamp);
    er_buf_str(&b, "] ");
    er_buf_str(&b, line);
    er_buf_str(&b, "\r\n");
    AcquireSRWLockExclusive(&log_lock);
    if (log_file != INVALID_HANDLE_VALUE && !b.failed) {
        DWORD written;
        WriteFile(log_file, b.data, (DWORD)b.length, &written, NULL);
    }
    ReleaseSRWLockExclusive(&log_lock);
    er_buf_free(&b);
}
#define log_format er_log

/* ---- songs (this start's, from er_generate) ---- */
static ErResult result;
static volatile LONG lookups, substitutions;

static int key_is_ours(const char *key) {
    static const char prefix[] = ER_KEY_PREFIX;
    for (size_t i = 0; i + 1 < sizeof(prefix); ++i)
        if (key[i] != prefix[i]) return 0;
    return 1;
}
static const ErSong *find_song(const char *key) {
    for (size_t i = 0; i < result.count; ++i)
        if (!strcmp(result.songs[i].key, key)) return &result.songs[i];
    return NULL;
}
static const ErSong *song_of_path(const char *name) {
    for (size_t i = 0; i < result.count; ++i)
        if (name == result.songs[i].path) return &result.songs[i];
    return NULL;
}

/* ---- FMOD 1.03.05 per-sound file callbacks (exinfo +0x78 open, +0x80 close, +0x88 read, +0x90 seek,
   +0xa8 fileuserdata; read by 0x1419b2b09 / 0x1419b2c03 and stored in the File at +0x200.. by 0x1419bb8c0).
   Signatures as the game's own callbacks 0x141006030..: open(name, unicode, filesize*, handle*, userdata),
   close(handle, userdata), read(handle, buffer, size, read*, userdata), seek(handle, pos, userdata).
   Results as the game's: 0 ok, 0x14 not found, 0xf read/seek failure. The game's own file layer cannot open
   host paths (live run 1), so our sounds read through CreateFileW (non-ASCII names work). */
#define EXINFO_FILE_OPEN 0x78
#define EXINFO_FILE_CLOSE 0x80
#define EXINFO_FILE_READ 0x88
#define EXINFO_FILE_SEEK 0x90
#define EXINFO_FILE_USERDATA 0xa8
static volatile LONG file_opens;

static int file_open(const char *name, int unicode, unsigned *filesize, void **handle, void *userdata) {
    (void)unicode;
    (void)name;
    const ErSong *song = (const ErSong *)userdata;
    ErFile *f = song ? er_open(song->path) : NULL;
    uint64_t size = f ? er_file_size(f) : 0;
    LONG n = InterlockedIncrement(&file_opens);
    if (n <= 400 || !f) log_format("open #%d %s -> %s (size %llu)", (int)n, song ? song->path : "(no song)", f ? "ok" : "FAIL", (unsigned long long)size);
    if (!f) return 0x14;
    if (filesize) *filesize = (unsigned)size;
    *handle = f;
    return 0;
}
typedef struct {
    HANDLE handle;
} SongFile; /* layout of struct ErFile in er_platform_win32.c */
static int file_close(void *handle, void *userdata) {
    (void)userdata;
    er_close((ErFile *)handle);
    return 0;
}
static int file_read(void *handle, void *buffer, unsigned size, unsigned *read, void *userdata) {
    (void)userdata;
    DWORD got = 0;
    if (!ReadFile(((SongFile *)handle)->handle, buffer, size, &got, NULL)) return 0xf;
    if (read) *read = got;
    return 0;
}
static int file_seek(void *handle, unsigned position, void *userdata) {
    (void)userdata;
    LARGE_INTEGER to;
    to.QuadPart = position;
    return SetFilePointerEx(((SongFile *)handle)->handle, to, NULL, FILE_BEGIN) ? 0 : 0xf;
}

/* ---- tracked sounds and streams (er_track.c), under one lock: createSound may run on FMOD's loader thread ---- */
static ErTracker tracker;
static SRWLOCK tracker_lock = SRWLOCK_INIT;
static volatile ULONGLONG skip_requested; /* tick of a pending request, 0: none (set by the poll thread) */

/* ---- hooks ---- */
static int hook_get_sound_info(void *studio, const char *key, unsigned char *info) {
    if (key && info && key_is_ours(key)) {
        const ErSong *song = find_song(key);
        LONG n = InterlockedIncrement(&lookups);
        if (!song) {
            log_format("FAIL: lookup #%d '%s': no such key this start", (int)n, key);
            return original_get_sound_info(studio, key, info);
        }
        memset(info, 0, INFO_BYTES);
        *(const char **)(info + INFO_NAME) = song->path;
        *(unsigned *)(info + INFO_MODE) = FMOD_ACCURATETIME;
        unsigned char *ex = info + INFO_EXINFO;
        *(int *)ex = EXINFO_SIZE;
        *(void **)(ex + EXINFO_FILE_OPEN) = (void *)&file_open;
        *(void **)(ex + EXINFO_FILE_CLOSE) = (void *)&file_close;
        *(void **)(ex + EXINFO_FILE_READ) = (void *)&file_read;
        *(void **)(ex + EXINFO_FILE_SEEK) = (void *)&file_seek;
        *(const void **)(ex + EXINFO_FILE_USERDATA) = song;
        *(int *)(info + INFO_SUBSOUND) = 0;
        if (n <= 400) log_format("lookup #%d '%s' -> %s - %s (%llu ms)", (int)n, key, song->artist, song->title, (unsigned long long)song->ms);
        return 0;
    }
    return original_get_sound_info(studio, key, info);
}

/* The song a createSound names: our own exinfo (file callbacks + userdata, set by the lookup above) and our path. */
static const ErSong *song_of_create(const char *name, const unsigned char *exinfo) {
    const ErSong *song = song_of_path(name);
    if (song && exinfo && *(const int *)exinfo >= EXINFO_SIZE && *(void *const *)(exinfo + EXINFO_FILE_OPEN) == (void *)&file_open) {
        const ErSong *own = *(const ErSong *const *)(exinfo + EXINFO_FILE_USERDATA);
        if (own && own != song) return NULL; /* cannot happen: the lookup sets both from one song */
    }
    return song;
}

static int hook_create_sound(void *system, const char *name, unsigned mode, void *exinfo, void **out) {
    int result_code = original_create_sound(system, name, mode, exinfo, out);
    const ErSong *song = song_of_create(name, (const unsigned char *)exinfo);
    void *sound = out && result_code == 0 ? *out : NULL;
    if (song && result_code) log_format("FAIL: createSound %s mode=0x%x -> result %d", song->key, mode, result_code);
    if (sound) {
        /* every new sound, ours or not: its address may be a released sound's, whose song it must never inherit */
        AcquireSRWLockExclusive(&tracker_lock);
        er_track_created(&tracker, sound, song);
        ReleaseSRWLockExclusive(&tracker_lock);
    }
    return result_code;
}

static int hook_get_open_state(void *sound, int *state, unsigned *percent, unsigned char *starving, unsigned char *busy) {
    int result_code = original_get_open_state(sound, state, percent, starving, busy);
    int now = state ? *state : -2, changed = 0;
    const ErSong *song = NULL;
    AcquireSRWLockExclusive(&tracker_lock);
    ErTrack *track = er_track_find(&tracker, sound);
    if (track && (now != track->state || result_code != track->result)) {
        track->state = now;
        track->result = result_code;
        changed = 1;
        song = track->song;
    }
    ReleaseSRWLockExclusive(&tracker_lock);
    if (changed && (now == 2 || result_code))
        log_format("FAIL: getOpenState %s -> result %d state %d (FMOD cannot open or decode it)", song ? song->key : "?", result_code, now);
    return result_code;
}

static int hook_get_sub_sound(void *sound, int index, void **out) {
    int count = -1;
    if (index == 0 && out && sound && get_num_sub_sounds(sound, &count) == 0 && count == 0) {
        *out = sound;
        unsigned length = 0;
        int lr = get_length(sound, &length, FMOD_TIMEUNIT_MS);
        LONG n = InterlockedIncrement(&substitutions);
        AcquireSRWLockExclusive(&tracker_lock);
        ErTrack *track = er_track_find(&tracker, sound);
        const ErSong *song = track ? track->song : NULL;
        if (track && lr == 0) track->length_ms = length;
        ReleaseSRWLockExclusive(&tracker_lock);
        if (n <= 400)
            log_format("getSubSound #%d %s: FMOD length %u ms (result %d), XML End %llu ms", (int)n, song ? song->key : "[not ours]", length, lr,
                       (unsigned long long)(song ? song->ms : 0));
        return 0;
    }
    return original_get_sub_sound(sound, index, out);
}

/* Runs on the radio update. Only the RadioStreamFmod::Update call site is trusted: there rdx is &stream->position. */
static int hook_get_timeline_position(void *instance, int *position) {
    int result_code = original_get_timeline_position(instance, position);
    if ((unsigned char *)__builtin_return_address(0) == game_base + RVA_UPDATE_POSITION_RETURN && position) {
        unsigned char *stream = (unsigned char *)position - STREAM_POSITION_OFFSET;
        void *sound = *(void **)(stream + STREAM_SOUND_OFFSET);
        int first = 0;
        AcquireSRWLockExclusive(&tracker_lock);
        const ErSong *song = er_track_seen(&tracker, stream, sound, *position, GetTickCount64(), &first);
        ReleaseSRWLockExclusive(&tracker_lock);
        /* once per sound: two streams update side by side around a track change */
        if (first) log_format("playing %s on station %d: %s - %s (%llu ms)", song->key, song->number, song->artist, song->title, (unsigned long long)song->ms);
    }
    return result_code;
}

/* Runs on the radio update, for every station, before the original. */
static void hook_station_update(unsigned char *station, void *context) {
    ULONGLONG requested = skip_requested;
    if (requested) {
        ULONGLONG now = GetTickCount64();
        int number = *(const int *)(station + STATION_NUMBER);
        float clock = *(const float *)(station + STATION_TRACK_CLOCK);
        ErSeen seen;
        AcquireSRWLockShared(&tracker_lock);
        int found = er_track_current(&tracker, number, (int)clock, now, 300, &seen);
        ReleaseSRWLockShared(&tracker_lock);
        if (found) {
            skip_requested = 0;
            const ErSong *song = seen.song;
            unsigned char *stream = (unsigned char *)seen.stream;
            int state = *(const int *)(station + STATION_STATE), sub = *(const int *)(station + STATION_SUBSTATE);
            int active = *(const unsigned char *)(station + STATION_TRACK_ACTIVE);
            int position = seen.position, target = er_skip_target(song, seen.length_ms);
            int diff = (int)clock - position;
            const char *why = now - requested > 1000                                       ? "the request is stale"
                              : state != 1 || sub != 4 || !active                             ? "the station is not in its plain track state"
                              : diff > 3000 || diff < -3000                                   ? "the station clock does not match the stream"
                              : *(void **)stream != game_base + RVA_STREAM_VTABLE             ? "not a RadioStreamFmod"
                              : (int)clock >= target - 1000 || position >= target - 1000      ? "already near End"
                                                                                              : NULL;
            if (why) {
                log_format("skip ignored on station %d (%s): state %d/%d active %d clock %d stream %d End %llu", song->number, why, state, sub, active,
                           (int)clock, position, (unsigned long long)song->ms);
            } else {
                static volatile LONG skips;
                LONG n = InterlockedIncrement(&skips);
                log_format("skip #%d %s: clock %d stream %d -> clock and Seek to %d (End %llu, FMOD %u)", (int)n, song->key, (int)clock, position, target,
                           (unsigned long long)song->ms, seen.length_ms);
                *(float *)(station + STATION_TRACK_CLOCK) = (float)target;
                ((StreamSeekFn)(game_base + RVA_STREAM_SEEK))(stream, target);
                log_format("skip #%d after: clock %d, stream start %d, stream state %d", (int)n, (int)*(const float *)(station + STATION_TRACK_CLOCK),
                           *(const int *)(stream + 0x28), *(const int *)(stream + 0x20));
            }
        } else if (GetTickCount64() - requested > 1000) {
            skip_requested = 0;
            log_format("skip ignored (the request is stale: no EasyRadio song plays now)");
        }
    }
    original_station_update(station, context);
}

/* ---- skip input: polled every 20 ms on our own thread; it only sets skip_requested ---- */
static int skip_vk;
static unsigned short skip_button;

static unsigned short game_buttons(void) {
    unsigned char *image = game_base;
    void *manager, *vtable, *controller;
    unsigned char connected;
    unsigned short buttons = 0;
    if (!memory_read(image + RVA_INPUT_MANAGER, &manager, sizeof(manager)) || !manager || !memory_read(manager, &vtable, sizeof(vtable)) ||
        vtable != image + RVA_INPUT_MANAGER_VTABLE || !memory_read((unsigned char *)manager + 0xab28, &controller, sizeof(controller)))
        return 0;
    uintptr_t first = (uintptr_t)manager + 0x10, selected = (uintptr_t)controller;
    if (selected < first || selected >= first + 8 * 0x1300 || (selected - first) % 0x1300 ||
        !memory_read((unsigned char *)controller + 4, &connected, 1) || connected != 1 ||
        !memory_read((unsigned char *)controller + 0xc0, &buttons, sizeof(buttons)))
        return 0;
    return buttons;
}

static int game_focused(void) {
    DWORD process = 0;
    HWND window = GetForegroundWindow();
    if (window) GetWindowThreadProcessId(window, &process);
    return process == GetCurrentProcessId();
}

static void request_skip(const char *source) {
    const ErSong *song;
    AcquireSRWLockShared(&tracker_lock);
    ULONGLONG tick = er_track_last_tick(&tracker, &song);
    ReleaseSRWLockShared(&tracker_lock);
    if (!song || GetTickCount64() - tick > 1000) {
        log_format("skip (%s) ignored: no EasyRadio song plays on the selected station", source);
        return;
    }
    skip_requested = GetTickCount64();
    log_format("skip requested (%s) while %s plays", source, song->key);
}

static DWORD WINAPI poll_thread(void *unused) {
    (void)unused;
    int key_was = 1; /* a key held at start does not count */
    unsigned short pad_was = 0xffff;
    ULONGLONG pad_down = 0;
    for (;;) {
        Sleep(20);
        int focused = game_focused();
        if (skip_vk) {
            int down = focused && (GetAsyncKeyState(skip_vk) & 0x8000) != 0;
            if (down && !key_was) request_skip("key");
            key_was = down;
        }
        if (skip_button) {
            unsigned short buttons = focused ? game_buttons() : 0;
            ULONGLONG now = GetTickCount64();
            int down = (buttons & skip_button) != 0, was = pad_was != 0xffff && (pad_was & skip_button);
            if (down && !was) pad_down = now;
            /* a tap: pressed alone, released within 400 ms */
            if (!down && was && now - pad_down <= 400 && !(pad_was & ~skip_button)) request_skip("pad");
            pad_was = buttons;
        }
    }
    return 0;
}

/* ---- RadioSystem::Load gate ---- */
enum { GENERATING = 0, READY = 1, STOCK = 2 };
static volatile LONG generation_state = GENERATING;
static volatile LONG load_seen;
static volatile ULONGLONG init_returned_tick; /* 0 until FH2PluginInitialize returns */
static wchar_t media_root[MAX_PATH * 2];
static volatile LONG media_published;

static void describe_language(void *language, char *out, size_t size) {
    /* MSVC std::string: inline buffer below 16 characters, else a pointer; length at +0x10, capacity at +0x18 */
    size_t length = 0, capacity = 0;
    const char *text = NULL;
    out[0] = 0;
    if (!language || !memory_read((unsigned char *)language + 0x10, &length, sizeof(length)) ||
        !memory_read((unsigned char *)language + 0x18, &capacity, sizeof(capacity)) || length >= size)
        return;
    if (capacity < 16) text = (const char *)language;
    else if (!memory_read(language, &text, sizeof(text))) return;
    if (!memory_read(text, out, length)) length = 0;
    out[length] = 0;
}

static void hook_radio_load(void *radio_system, void *banks, void *language) {
    InterlockedExchange(&load_seen, 1);
    char lang[16];
    describe_language(language, lang, sizeof(lang));
    ULONGLONG start = GetTickCount64();
    LONG state = generation_state;
    if (state == GENERATING) log_format("RadioSystem::Load (language %s) waits for the generated RadioInfo", lang);
    while ((state = generation_state) == GENERATING && GetTickCount64() - start < 30000) Sleep(5);
    /* ExtraOptions publishes the private media root right after FH2PluginInitialize returns */
    if (state == READY) {
        while (!init_returned_tick && GetTickCount64() - start < 30000) Sleep(5);
        while (init_returned_tick && GetTickCount64() - init_returned_tick < 250) Sleep(5);
    }
    log_format("RadioSystem::Load (language %s) after %llu ms: %s", lang, (unsigned long long)(GetTickCount64() - start),
               state == READY      ? "reads the generated RadioInfo_<lang>.xml"
               : state == STOCK    ? "stock RadioInfo (nothing generated)"
                                   : "TIMEOUT waiting for the generator: stock RadioInfo");
    original_radio_load(radio_system, banks, language);
}

/* 8-byte pointer write into the vtable: atomic, no thread freeze needed */
static int patch_pointer(void **slot, void *value) {
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) return 0;
    __atomic_store_n(slot, value, __ATOMIC_SEQ_CST);
    VirtualProtect(slot, sizeof(void *), old, &old);
    return 1;
}

/* ---- hook install (FH Mods Collection, GPL-3.0-or-later; see NOTICE) ---- */
#define FROZEN_CAPACITY 1024
static HANDLE frozen[FROZEN_CAPACITY];
static unsigned frozen_count;
static CONTEXT frozen_context;

static void freeze_threads(const unsigned char *target, size_t length, const unsigned char *trampoline) {
    frozen_count = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    DWORD process = GetCurrentProcessId(), self = GetCurrentThreadId();
    THREADENTRY32 entry;
    entry.dwSize = sizeof(entry);
    for (BOOL more = Thread32First(snapshot, &entry); more && frozen_count < FROZEN_CAPACITY;
         entry.dwSize = sizeof(entry), more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self) continue;
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, entry.th32ThreadID);
        if (!thread) continue;
        if (SuspendThread(thread) == (DWORD)-1) {
            CloseHandle(thread);
            continue;
        }
        frozen[frozen_count++] = thread;
        frozen_context.ContextFlags = CONTEXT_CONTROL;
        uintptr_t start = (uintptr_t)target;
        if (GetThreadContext(thread, &frozen_context) && frozen_context.Rip >= start && frozen_context.Rip < start + length) {
            frozen_context.Rip = (uintptr_t)trampoline + (frozen_context.Rip - start);
            SetThreadContext(thread, &frozen_context);
        }
    }
    CloseHandle(snapshot);
}

static void thaw_threads(void) {
    while (frozen_count) {
        HANDLE thread = frozen[--frozen_count];
        ResumeThread(thread);
        CloseHandle(thread);
    }
}

/* copy_len whole position-independent prologue bytes run from the trampoline. The jump written at target is
   the 14-byte jmp [rip+0] when copy_len >= 14, else the 12-byte mov rax, imm64; jmp rax (rax is free at a
   function's entry). The trampoline is published in *original before the jump exists. */
static int install_hook(void **original, unsigned char *target, size_t copy_len, void *hook) {
    unsigned char *trampoline = (unsigned char *)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!trampoline) return 0;
    static const unsigned char jmp_rip[6] = {0xff, 0x25, 0x00, 0x00, 0x00, 0x00};
    memcpy(trampoline, target, copy_len);
    memcpy(trampoline + copy_len, jmp_rip, sizeof(jmp_rip));
    uintptr_t back = (uintptr_t)(target + copy_len);
    memcpy(trampoline + copy_len + 6, &back, sizeof(back));
    DWORD old;
    if (!VirtualProtect(trampoline, 64, PAGE_EXECUTE_READ, &old)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return 0;
    }
    FlushInstructionCache(GetCurrentProcess(), trampoline, 64);
    DWORD target_protect;
    if (!VirtualProtect(target, copy_len, PAGE_EXECUTE_READWRITE, &target_protect)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return 0;
    }
    __atomic_store_n(original, (void *)trampoline, __ATOMIC_RELEASE);
    unsigned char patch[16];
    uintptr_t to = (uintptr_t)hook;
    size_t used;
    if (copy_len >= 14) {
        memcpy(patch, jmp_rip, 6);
        memcpy(patch + 6, &to, 8);
        used = 14;
    } else {
        patch[0] = 0x48;
        patch[1] = 0xb8;
        memcpy(patch + 2, &to, 8);
        patch[10] = 0xff;
        patch[11] = 0xe0;
        used = 12;
    }
    for (size_t i = used; i < copy_len; ++i) patch[i] = 0x90;
    freeze_threads(target, copy_len, trampoline);
    memcpy(target, patch, copy_len);
    FlushInstructionCache(GetCurrentProcess(), target, copy_len);
    thaw_threads();
    VirtualProtect(target, copy_len, target_protect, &target_protect);
    return 1;
}

/* ---- paths ---- */
static char *utf8_of(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = n > 0 ? er_alloc((size_t)n) : NULL;
    if (s) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

/* The game's folder, from Azimuth's own settings: <azimuth.exe dir>\data\azimuth.ini [title-1a96545b] folder= */
static char *azimuth_game_folder(void) {
    static wchar_t exe[MAX_PATH * 2], ini[MAX_PATH * 2], folder[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH * 2);
    if (!n || n >= MAX_PATH * 2) return NULL;
    while (n && exe[n - 1] != L'\\' && exe[n - 1] != L'/') --n;
    exe[n] = 0;
    static const wchar_t tail[] = L"data\\azimuth.ini";
    if (n + sizeof(tail) / sizeof(wchar_t) >= MAX_PATH * 2) return NULL;
    for (DWORD i = 0; i <= n; ++i) ini[i] = exe[i];
    for (DWORD i = 0; i < sizeof(tail) / sizeof(wchar_t); ++i) ini[n + i] = tail[i];
    if (!GetPrivateProfileStringW(L"title-1a96545b", L"folder", L"", folder, MAX_PATH * 2, ini) || !folder[0]) return NULL;
    return utf8_of(folder);
}

/* The player's radio folders: <azimuth.exe dir>\\EasyRadio\\, outside the mod's folder, so removing or updating the mod
   never touches the music (UTF-8, with the trailing separator). */
static char *music_folder(void) {
    static wchar_t exe[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH * 2);
    if (!n || n >= MAX_PATH * 2) return NULL;
    while (n && exe[n - 1] != L'\\' && exe[n - 1] != L'/') --n;
    exe[n] = 0;
    char *dir = utf8_of(exe);
    char *music = dir ? er_join(dir, "EasyRadio\\", NULL) : NULL;
    er_free(dir);
    return music;
}

/* ---- entry points ---- */
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)instance;
    (void)reason;
    (void)reserved;
    return TRUE;
}

__declspec(dllexport) const wchar_t *WINAPI FH2PluginMediaDirectory(void) {
    if (generation_state != READY) return NULL;
    InterlockedExchange(&media_published, 1);
    return media_root;
}

static wchar_t mod_dir_w[MAX_PATH * 2]; /* with trailing backslash */

static int open_log(const wchar_t *plugins_directory) {
    size_t n = 0;
    while (plugins_directory[n]) ++n;
    while (n && (plugins_directory[n - 1] == L'\\' || plugins_directory[n - 1] == L'/')) --n;
    size_t start = n;
    while (start && plugins_directory[start - 1] != L'\\' && plugins_directory[start - 1] != L'/') --start;
    if (!start || start + 64 >= MAX_PATH * 2) return 0;
    for (size_t i = 0; i < start; ++i) mod_dir_w[i] = plugins_directory[i]; /* keeps the separator before "plugins" */
    mod_dir_w[start] = 0;
    static wchar_t log_path[MAX_PATH * 2];
    static const wchar_t name[] = L"EasyRadio.log";
    for (size_t i = 0; i < start; ++i) log_path[i] = mod_dir_w[i];
    for (size_t i = 0; i < sizeof(name) / sizeof(wchar_t); ++i) log_path[start + i] = name[i];
    log_file = CreateFileW(log_path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    /* the private media root: <mod>\plugins\generated (ExtraOptions requires it below plugins_directory) */
    static const wchar_t generated[] = L"\\generated";
    for (size_t i = 0; i < n; ++i) media_root[i] = plugins_directory[i];
    for (size_t i = 0; i < sizeof(generated) / sizeof(wchar_t); ++i) media_root[n + i] = generated[i];
    return log_file != INVALID_HANDLE_VALUE;
}

static BOOL finish(const FH2PluginContext *context, BOOL ok, const char *message) {
    if (context->log_utf8) context->log_utf8(message);
    init_returned_tick = GetTickCount64();
    return ok;
}

__declspec(dllexport) BOOL WINAPI FH2PluginInitialize(const FH2PluginContext *context) {
    if (!context || context->size < sizeof(*context) || context->api_version != FH2_PLUGIN_API_VERSION || !context->plugins_directory) return FALSE;
    if (!open_log(context->plugins_directory)) {
        generation_state = STOCK;
        return finish(context, FALSE, "EasyRadio: cannot open EasyRadio.log; not started.\r\n");
    }
    unsigned char *game = (unsigned char *)context->game_image;
    log_format("EasyRadio %s: ExtraOptions %s, adapter %u, image %p size 0x%x", EASYRADIO_VERSION,
               context->extra_options_version ? context->extra_options_version : "?", context->adapter, game, context->game_image_size);
    if (context->adapter != FH2_PLUGIN_AZIMUTH || !game || context->game_image_size != GAME_IMAGE_SIZE) {
        log_format("FAIL: not Azimuth or not the supported FH2 image (size 0x%x, want 0x%x); nothing hooked", context->game_image_size, GAME_IMAGE_SIZE);
        generation_state = STOCK;
        return finish(context, FALSE, "EasyRadio: unsupported game image; not started.\r\n");
    }
    game_base = game;
    struct {
        const char *name;
        unsigned rva;
        const unsigned char *prologue;
        size_t length;
    } checks[] = {
        {"getSoundInfo", RVA_GET_SOUND_INFO, prologue_get_sound_info, sizeof(prologue_get_sound_info)},
        {"getSubSound", RVA_GET_SUB_SOUND, prologue_get_sub_sound, sizeof(prologue_get_sub_sound)},
        {"createSound", RVA_CREATE_SOUND, prologue_create_sound, sizeof(prologue_create_sound)},
        {"getOpenState", RVA_GET_OPEN_STATE, prologue_get_open_state, sizeof(prologue_get_open_state)},
        {"getTimelinePosition", RVA_GET_TIMELINE_POSITION, prologue_get_timeline_position, sizeof(prologue_get_timeline_position)},
        {"RadioStation::Update", RVA_STATION_UPDATE, prologue_station_update, sizeof(prologue_station_update)},
        {"RadioStreamFmod::Seek", RVA_STREAM_SEEK, prologue_stream_seek, sizeof(prologue_stream_seek)},
        {"RadioStreamFmod::Update position call", RVA_UPDATE_POSITION_RETURN - 9, prologue_update_position_call, sizeof(prologue_update_position_call)},
        {"RadioSystem::Load", RVA_RADIO_SYSTEM_LOAD, prologue_radio_load, sizeof(prologue_radio_load)},
        {"AudioManager RadioSystem::Load call", RVA_RADIO_LOAD_CALL, prologue_radio_load_call, sizeof(prologue_radio_load_call)},
    };
    for (unsigned i = 0; i < sizeof(checks) / sizeof(checks[0]); ++i) {
        if (!memory_equal(game + checks[i].rva, checks[i].prologue, checks[i].length)) {
            log_format("FAIL: code of %s at rva 0x%x differs (another game build, or another mod's hook); nothing hooked", checks[i].name, checks[i].rva);
            generation_state = STOCK;
            return finish(context, FALSE, "EasyRadio: unexpected game code; not started.\r\n");
        }
    }
    void **load_slot = (void **)(game + RVA_RADIO_SYSTEM_LOAD_SLOT);
    if (*load_slot != game + RVA_RADIO_SYSTEM_LOAD || *(unsigned char **)(game + RVA_STREAM_VTABLE + 0x58) != game + RVA_STREAM_SEEK) {
        log_format("FAIL: RadioSystem vtable +0x08 is not Load, or RadioStreamFmod vtable +0x58 is not Seek; nothing hooked");
        generation_state = STOCK;
        return finish(context, FALSE, "EasyRadio: unexpected game data; not started.\r\n");
    }
    log_format("code checks ok (%u)", (unsigned)(sizeof(checks) / sizeof(checks[0])));

    /* 1. the Load gate first: the radio must not read RadioInfo before it is generated */
    original_radio_load = (RadioLoadFn)(game + RVA_RADIO_SYSTEM_LOAD);
    int gate = patch_pointer(load_slot, (void *)&hook_radio_load);
    void *radio_system = *(void *volatile *)(game + RVA_RADIO_SYSTEM);
    log_format("RadioSystem::Load gate %s; radio %s", gate ? "installed" : "FAILED",
               radio_system ? "ALREADY CREATED (Load may have run before this plugin)" : "not loaded yet (good)");

    /* 2. generate */
    char *mod_dir = utf8_of(mod_dir_w);
    char *root = utf8_of(media_root);
    char *out_dir = root ? er_join(root, "\\", NULL) : NULL;
    char *cache_dir = mod_dir ? er_join(mod_dir, "plugins\\cache\\", NULL) : NULL;
    char *game_folder = azimuth_game_folder();
    char *music_dir = music_folder();
    const char *game_dirs[2] = {game_folder, NULL};
    log_format("mod %s; music %s; game folder (Azimuth settings) %s", mod_dir ? mod_dir : "?", music_dir ? music_dir : "?",
               game_folder ? game_folder : "(not found)");
    int ok = 0;
    if (mod_dir && out_dir && cache_dir && er_make_dirs(out_dir)) {
        ULONGLONG t0 = GetTickCount64();
        ErOptions options = {mod_dir, out_dir, cache_dir, game_dirs, '\\', 1, er_random_seed(), music_dir};
        ok = gate && er_generate(&options, &result);
        log_format("generated in %llu ms: %u songs, %u RadioInfo languages, %u HUD icons", (unsigned long long)(GetTickCount64() - t0),
                   (unsigned)result.count, result.languages, result.logos);
        for (size_t i = 0; ok && i < result.count; ++i)
            log_format("  %s %llu ms  %s - %s", result.songs[i].key, (unsigned long long)result.songs[i].ms, result.songs[i].artist, result.songs[i].title);
    } else {
        log_format("FAIL: cannot create %s", out_dir ? out_dir : "the generated folder");
    }
    er_free(mod_dir);
    er_free(root);
    er_free(out_dir);
    er_free(cache_dir);
    er_free(game_folder);
    er_free(music_dir);

    /* 3. too late? Load ran (or is running) without our gate: publish nothing, the radio stays stock */
    radio_system = *(void *volatile *)(game + RVA_RADIO_SYSTEM);
    if (ok && radio_system && !load_seen) {
        log_format("FAIL: the radio loaded before EasyRadio could start (RadioSystem::Load ran ungated); stock radio this start. "
                   "Tell the EasyRadio maintainer: the load order assumption is broken.");
        ok = 0;
    }
    if (!ok) {
        generation_state = STOCK;
        log_format("EasyRadio is off for this start: the radio stays stock");
        return finish(context, TRUE, "EasyRadio: nothing to play; the radio stays stock.\r\n");
    }

    /* 4. playback hooks, downstream first so the first answered key already has its subsound fix */
    get_num_sub_sounds = (GetNumSubSoundsFn)(game + RVA_GET_NUM_SUB_SOUNDS);
    get_length = (GetLengthFn)(game + RVA_GET_LENGTH);
    int hooks = install_hook((void **)&original_get_sub_sound, game + RVA_GET_SUB_SOUND, sizeof(prologue_get_sub_sound), (void *)&hook_get_sub_sound) &&
                install_hook((void **)&original_get_open_state, game + RVA_GET_OPEN_STATE, sizeof(prologue_get_open_state), (void *)&hook_get_open_state) &&
                install_hook((void **)&original_create_sound, game + RVA_CREATE_SOUND, sizeof(prologue_create_sound), (void *)&hook_create_sound) &&
                install_hook((void **)&original_get_sound_info, game + RVA_GET_SOUND_INFO, sizeof(prologue_get_sound_info), (void *)&hook_get_sound_info);
    int skip_ok = hooks &&
                  install_hook((void **)&original_get_timeline_position, game + RVA_GET_TIMELINE_POSITION, sizeof(prologue_get_timeline_position),
                               (void *)&hook_get_timeline_position) &&
                  install_hook((void **)&original_station_update, game + RVA_STATION_UPDATE, sizeof(prologue_station_update), (void *)&hook_station_update);
    if (!hooks) {
        /* without getSoundInfo our keys would play silence: do not publish the generated RadioInfo */
        generation_state = STOCK;
        log_format("FAIL: a playback hook could not be installed; the radio stays stock");
        return finish(context, TRUE, "EasyRadio: hook install failed; the radio stays stock.\r\n");
    }
    skip_vk = result.config.skip_vk;
    skip_button = (unsigned short)result.config.skip_button;
    HANDLE thread = skip_ok && (skip_vk || skip_button) ? CreateThread(NULL, 0, poll_thread, NULL, 0, NULL) : NULL;
    if (thread) CloseHandle(thread);
    log_format("skip: key %s%u, pad button 0x%x; %s", skip_vk ? "F" : "off ", skip_vk ? (unsigned)(skip_vk - VK_F1 + 1) : 0u, skip_button,
               skip_ok ? (thread ? "on" : "off") : "hook FAILED");
    generation_state = READY;
    char *shown = utf8_of(media_root);
    log_format("ready: hooks installed; %u songs; private media root %s", (unsigned)result.count, shown ? shown : "?");
    er_free(shown);
    return finish(context, TRUE, "EasyRadio: ready.\r\n");
}
