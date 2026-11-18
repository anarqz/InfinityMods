/* EasyRadio core: everything that does not touch the game. Platform-free C (no CRT calls beyond memcpy/memset,
   which the plugin defines itself); the same files build the Windows plugin and the Linux host tests.
   The platform (er_platform_win32.c in the plugin, tests/er_platform_posix.c on the host) supplies the
   functions declared under "platform". */
#pragma once
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

/* ---- platform ---- */
void *er_alloc(size_t size); /* zeroed; NULL on failure */
void er_free(void *memory);
void er_log_line(const char *line);       /* one line, no newline */
typedef struct ErFile ErFile;
ErFile *er_open(const char *path);        /* UTF-8 path; NULL when it cannot be opened */
uint64_t er_file_size(ErFile *file);
int er_read_at(ErFile *file, uint64_t offset, void *buffer, size_t size); /* 1 when all size bytes were read */
void er_close(ErFile *file);
int er_write_file(const char *path, const void *data, size_t size);       /* creates or replaces; 1 on success */
int er_make_dirs(const char *path);       /* every directory of path; 1 on success */
int er_delete_file(const char *path);     /* 1 when it no longer exists */
/* calls visit(name, is_dir, size, mtime, context) for every entry but . and ..; 0 when the folder cannot be read */
typedef void (*ErVisit)(const char *name, int is_dir, uint64_t size, uint64_t mtime, void *context);
int er_list_dir(const char *path, ErVisit visit, void *context);
uint64_t er_random_seed(void);

/* ---- memory and buffers ---- */
void *er_realloc(void *memory, size_t old_size, size_t new_size);
typedef struct {
    char *data;
    size_t length, capacity;
    int failed; /* an allocation failed: the content is incomplete */
} ErBuf;
void er_buf_bytes(ErBuf *buf, const void *data, size_t length);
void er_buf_str(ErBuf *buf, const char *text);
void er_buf_char(ErBuf *buf, char c);
void er_buf_uint(ErBuf *buf, uint64_t value);
void er_buf_free(ErBuf *buf);
const char *er_buf_cstr(ErBuf *buf); /* NUL-terminated view (adds a NUL past length) */
/* %s %d %u %x %llu %c; also used by er_log */
void er_buf_vformat(ErBuf *buf, const char *format, va_list args);
void er_buf_format(ErBuf *buf, const char *format, ...);
void er_log(const char *format, ...);

size_t er_strlen(const char *text);
int er_strcmp(const char *a, const char *b);
char *er_strdup(const char *text, size_t length);
int er_equal_ci(const char *a, size_t length, const char *b); /* ASCII, b NUL-terminated */
char *er_join(const char *a, const char *b, const char *c); /* concatenation, freed with er_free */

/* ---- Unicode text (er_text.c) ---- */
typedef struct {
    uint32_t *cp;
    size_t n, capacity;
} ErText;
void er_text_free(ErText *t);
void er_text_push(ErText *t, uint32_t cp);
void er_text_from_utf8_lossy(ErText *t, const char *s, size_t n); /* invalid bytes become U+FFFD */
void er_text_to_utf8(const ErText *t, ErBuf *out);
/* Python's codecs, strict: 0 on a decode error (the prototype then uses ""). */
int er_decode_latin1(ErText *t, const uint8_t *s, size_t n);
int er_decode_utf8(ErText *t, const uint8_t *s, size_t n);
int er_decode_utf16(ErText *t, const uint8_t *s, size_t n, int big_endian, int bom_detect);
int er_is_space(uint32_t cp);
int er_is_word(uint32_t cp);
void er_text_lower(const ErText *in, ErText *out);
/* ascii_text: the HUD font is Latin-only. out gets printable ASCII. */
void er_ascii_text(const ErText *in, ErBuf *out);
#define ER_TITLE_CHARS 80
#define ER_ARTIST_CHARS 40
/* hud_names: (artist, title) from the file name (without .mp3) and the TIT2/TPE1 tags (n=0: absent).
   artist/title receive NUL-terminated ASCII. */
void er_hud_names(const ErText *stem, const ErText *title_tag, const ErText *artist_tag, ErBuf *artist, ErBuf *title);
void er_strip_noise(const ErText *in, ErText *out); /* exposed for tests */

/* ---- MP3 (er_mp3.c) ---- */
typedef struct {
    ErText title, artist; /* TIT2 / TPE1, n=0 when absent */
} ErTags;
void er_tags_free(ErTags *tags);
void er_id3v2_text(const uint8_t *data, size_t size, ErTags *tags);
void er_id3v1_text(const uint8_t *data, size_t size, ErTags *tags);
/* bytes the tag(s) at the start occupy (skip_id3v2) */
size_t er_id3v2_skip(const uint8_t *data, size_t size);
/* mp3_duration_ms on the whole file: 1 with *ms, 0 when there are no MPEG audio frames */
int er_mp3_duration_ms(const uint8_t *data, size_t size, uint64_t *ms);
/* The same, from a prefix of a file of total_size bytes: 1 done, 0 no frames, -1 needs the whole file. */
int er_mp3_duration_prefix(const uint8_t *data, size_t length, uint64_t total_size, uint64_t *ms);

/* ---- config (er_ini.c) ---- */
typedef struct {
    const char *id;        /* pulse ... innovative */
    const char *name;      /* RadioStation Name in RadioInfo */
    int number;            /* RadioStation Number */
    const char *icon;      /* HUD_RadioIcon_<icon>.swatchbin */
} ErStation;
#define ER_STATIONS 7
extern const ErStation er_stations[ER_STATIONS];
int er_station_index(const char *id, size_t length); /* -1 if unknown; "horizon" = pulse */
int er_is_absolute(const char *path, size_t length); /* C:\\..., C:/..., \\\\server\\..., or /... */
typedef struct {
    char *folder[ER_STATIONS]; /* NULL: the station stays stock */
    int dj_off;
    int skip_vk;               /* 0 off, else 0x70 + n - 1 (VK_F1..VK_F12) */
    unsigned skip_button;      /* 0 off */
    char *game_folder;         /* optional override of the game's Mount folder */
    int errors;                /* lines that were logged and ignored */
} ErConfig;
void er_config_defaults(ErConfig *config);
/* parses easyradio.ini text; problems are logged and that line ignored */
void er_config_parse(ErConfig *config, const char *text, size_t length);
void er_config_free(ErConfig *config);

/* ---- RadioInfo XML (er_xml.c) ---- */
typedef struct {
    char key[40];
    char *artist, *title; /* ASCII */
    uint64_t ms;
} ErEntry;
/* Replaces the station's FreeroamTracks and EventTracks (and with dj_off its Stingers, ShortStingers, DJ) in a
   RadioInfo text; the rest stays byte for byte. 1 on success, 0 (logged) when the station or a list is missing. */
int er_edit_station(const char *text, size_t length, const char *station, const ErEntry *entries, size_t count, int dj_off,
                    ErBuf *out);

/* ---- shuffle ---- */
typedef struct {
    uint64_t state;
} ErRandom;
uint64_t er_random_next(ErRandom *random);
void er_shuffle(void *items, size_t count, size_t size, ErRandom *random);
void er_sort(void *items, size_t count, size_t size, int (*less)(const void *, const void *));

/* ---- logo (er_logo.c) ---- */
#define ER_SWATCH_SIZE 32851u
/* image file bytes (PNG, JPEG, BMP) -> HUD_RadioIcon swatchbin (ER_SWATCH_SIZE bytes into out). 1 on success; the
   encoding is decoded back and checked before it counts. */
int er_logo_swatch(const uint8_t *image, size_t size, uint8_t *out, char *why, size_t why_size);
/* decodes a swatchbin's 196x96 logo area back to RGBA (tests) */
int er_swatch_decode(const uint8_t *swatch, uint8_t *rgba_256x128);
