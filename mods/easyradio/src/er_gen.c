/* EasyRadio generator (see er_gen.h). Every problem is logged and skipped: a bad file loses that song, a bad
   folder that station, and with nothing valid the radio stays stock. The game files it reads (RadioInfo XML) and
   the files it writes stay on the player's machine. */
#include "er_gen.h"

#include <string.h>

const char *const er_languages[ER_LANGUAGES] = {"BR", "DE", "EN", "ES", "FR", "IT", "JP", "MX", "PL", "RU"};

#define CACHE_MAGIC "easyradio-cache 1"
#define HEAD_BYTES (512u * 1024u)
#define AFTER_TAGS (256u * 1024u)
#define MAX_FILE (1024ull * 1024ull * 1024ull) /* a whole-file scan reads at most 1 GiB */
#define MAX_SONGS_PER_STATION 4000u

/* ---- file helpers ---- */
static int read_whole(const char *path, ErBuf *out, uint64_t limit) {
    ErFile *f = er_open(path);
    if (!f) return 0;
    uint64_t size = er_file_size(f);
    int ok = size <= limit;
    if (ok && size) {
        char *data = er_alloc((size_t)size + 1);
        ok = data && er_read_at(f, 0, data, (size_t)size);
        if (ok) {
            er_buf_free(out);
            out->data = data;
            out->length = (size_t)size;
            out->capacity = (size_t)size + 1;
        } else {
            er_free(data);
        }
    }
    er_close(f);
    return ok;
}

static char *path3(const char *a, const char *b, const char *c) {
    return er_join(a, b, c);
}

/* ---- folder listing ---- */
typedef struct {
    char *name;
    uint64_t size, mtime;
    ErText lower; /* sort key: Python's name.lower() */
} Item;
typedef struct {
    Item *items;
    size_t count, capacity;
    char *logo; /* logo.png / .jpg / .jpeg / .bmp */
} Listing;

static int ends_ci(const char *name, size_t n, const char *suffix) {
    size_t k = er_strlen(suffix);
    return n >= k && er_equal_ci(name + n - k, k, suffix);
}
static void visit(const char *name, int is_dir, uint64_t size, uint64_t mtime, void *context) {
    Listing *l = context;
    size_t n = er_strlen(name);
    if (is_dir) return;
    static const char *const logos[] = {"logo.png", "logo.jpg", "logo.jpeg", "logo.bmp"};
    for (size_t i = 0; i < 4; ++i)
        if (er_equal_ci(name, n, logos[i]) && (!l->logo || i == 0)) {
            er_free(l->logo);
            l->logo = er_strdup(name, n);
        }
    if (!ends_ci(name, n, ".mp3") || n <= 4) return;
    if (l->count == l->capacity) {
        size_t grown = l->capacity ? l->capacity * 2 : 64;
        Item *more = er_realloc(l->items, l->capacity * sizeof(Item), grown * sizeof(Item));
        if (!more) return;
        l->items = more;
        l->capacity = grown;
    }
    Item *it = &l->items[l->count++];
    memset(it, 0, sizeof(*it));
    it->name = er_strdup(name, n);
    it->size = size;
    it->mtime = mtime;
    ErText t = {0};
    er_text_from_utf8_lossy(&t, name, n);
    er_text_lower(&t, &it->lower);
    er_text_free(&t);
}
static int item_less(const void *a, const void *b) {
    const ErText *x = &((const Item *)a)->lower, *y = &((const Item *)b)->lower;
    for (size_t i = 0; i < x->n && i < y->n; ++i)
        if (x->cp[i] != y->cp[i]) return x->cp[i] < y->cp[i];
    return x->n < y->n;
}
static void listing_free(Listing *l) {
    for (size_t i = 0; i < l->count; ++i) {
        er_free(l->items[i].name);
        er_text_free(&l->items[i].lower);
    }
    er_free(l->items);
    er_free(l->logo);
    memset(l, 0, sizeof(*l));
}

/* ---- the duration and tag cache: one line per file, keyed by folder/name, size and mtime ---- */
typedef struct {
    char *file; /* folder/name */
    uint64_t size, mtime, ms;
    char *artist, *title; /* ms 0: not playable; title holds the reason */
    int used;
} CacheEntry;
typedef struct {
    CacheEntry *entries;
    size_t count, capacity;
} Cache;

static void cache_add(Cache *c, const char *file, uint64_t size, uint64_t mtime, uint64_t ms, const char *artist, const char *title, int used) {
    if (c->count == c->capacity) {
        size_t grown = c->capacity ? c->capacity * 2 : 256;
        CacheEntry *more = er_realloc(c->entries, c->capacity * sizeof(CacheEntry), grown * sizeof(CacheEntry));
        if (!more) return;
        c->entries = more;
        c->capacity = grown;
    }
    CacheEntry *e = &c->entries[c->count++];
    e->file = er_strdup(file, er_strlen(file));
    e->size = size;
    e->mtime = mtime;
    e->ms = ms;
    e->artist = er_strdup(artist, er_strlen(artist));
    e->title = er_strdup(title, er_strlen(title));
    e->used = used;
}
static uint64_t parse_u64(const char *p, const char *end) {
    uint64_t v = 0;
    for (; p < end && *p >= '0' && *p <= '9'; ++p) v = v * 10 + (uint64_t)(*p - '0');
    return v;
}
static void cache_load(Cache *c, const char *path) {
    ErBuf text = {0};
    if (!read_whole(path, &text, 64ull * 1024 * 1024)) return;
    const char *p = text.data, *end = text.data + text.length;
    size_t magic = er_strlen(CACHE_MAGIC);
    if (text.length < magic || memcmp(p, CACHE_MAGIC, magic)) {
        er_buf_free(&text);
        return;
    }
    while (p < end) {
        const char *line_end = p;
        while (line_end < end && *line_end != '\n') ++line_end;
        const char *field[6];
        size_t length[6], n = 0;
        const char *f = p;
        for (const char *q = p; q <= line_end && n < 6; ++q) {
            if (q == line_end || *q == '\t') {
                field[n] = f;
                length[n++] = (size_t)(q - f);
                f = q + 1;
            }
        }
        if (n == 6) {
            char *file = er_strdup(field[0], length[0]), *artist = er_strdup(field[4], length[4]), *title = er_strdup(field[5], length[5]);
            if (file && artist && title)
                cache_add(c, file, parse_u64(field[1], field[1] + length[1]), parse_u64(field[2], field[2] + length[2]),
                          parse_u64(field[3], field[3] + length[3]), artist, title, 0);
            er_free(file);
            er_free(artist);
            er_free(title);
        }
        p = line_end < end ? line_end + 1 : end;
    }
    er_buf_free(&text);
}
static CacheEntry *cache_find(Cache *c, const char *file, uint64_t size, uint64_t mtime) {
    for (size_t i = 0; i < c->count; ++i)
        if (c->entries[i].size == size && c->entries[i].mtime == mtime && !er_strcmp(c->entries[i].file, file)) return &c->entries[i];
    return NULL;
}
static void cache_free(Cache *c) {
    for (size_t i = 0; i < c->count; ++i) {
        er_free(c->entries[i].file);
        er_free(c->entries[i].artist);
        er_free(c->entries[i].title);
    }
    er_free(c->entries);
    memset(c, 0, sizeof(*c));
}
static int cacheable(const char *text) {
    for (; *text; ++text)
        if (*text == '\t' || *text == '\n' || *text == '\r') return 0;
    return 1;
}

/* ---- one MP3: tags, HUD text, duration ---- */
static const char *scan_song(const char *path, const char *name, uint64_t *ms, ErBuf *artist, ErBuf *title) {
    ErFile *f = er_open(path);
    if (!f) return "cannot open the file";
    uint64_t size = er_file_size(f);
    const char *why = NULL;
    size_t want = size < HEAD_BYTES ? (size_t)size : HEAD_BYTES;
    uint8_t *head = er_alloc(want + 1), *whole = NULL;
    uint8_t tail[128];
    ErTags tags = {{0}, {0}};
    if (!head || (want && !er_read_at(f, 0, head, want))) {
        why = "cannot read the file";
        goto done;
    }
    size_t skip = er_id3v2_skip(head, want);
    if (skip + AFTER_TAGS > want && want < size) { /* large tags (cover art): read past them */
        uint64_t more = skip + AFTER_TAGS;
        if (more > size) more = size;
        if (more > MAX_FILE) {
            why = "tags too large";
            goto done;
        }
        er_free(head);
        want = (size_t)more;
        head = er_alloc(want + 1);
        if (!head || !er_read_at(f, 0, head, want)) {
            why = "cannot read the file";
            goto done;
        }
    }
    if (size >= 128 && er_read_at(f, size - 128, tail, 128)) er_id3v1_text(tail, 128, &tags);
    er_id3v2_text(head, want, &tags);
    int found = er_mp3_duration_prefix(head, want, size, ms);
    if (found < 0) {
        if (size > MAX_FILE) {
            why = "too large for a frame scan (over 1 GiB without a Xing/VBRI header)";
            goto done;
        }
        whole = er_alloc((size_t)size + 1);
        if (!whole || !er_read_at(f, 0, whole, (size_t)size)) {
            why = "cannot read the whole file";
            goto done;
        }
        found = er_mp3_duration_ms(whole, (size_t)size, ms);
    }
    if (!found) {
        why = "no MPEG audio frames";
        goto done;
    }
    if (*ms < ER_MIN_SONG_MS) {
        why = "shorter than 10 s";
        goto done;
    }
    {
        size_t n = er_strlen(name);
        ErText stem = {0};
        er_text_from_utf8_lossy(&stem, name, n >= 4 ? n - 4 : n);
        er_hud_names(&stem, &tags.title, &tags.artist, artist, title);
        er_text_free(&stem);
    }
done:
    er_tags_free(&tags);
    er_free(head);
    er_free(whole);
    er_close(f);
    return why;
}

/* ---- stations ---- */
typedef struct {
    ErSong *songs;
    size_t count;
} StationSongs;

static int song_key(char *key, size_t size, int station, size_t n) {
    ErBuf b = {0};
    er_buf_format(&b, "%s%s_%u", ER_KEY_PREFIX, er_stations[station].id, (unsigned)n);
    int ok = b.length < size && !b.failed;
    if (ok) memcpy(key, b.data, b.length + 1);
    er_buf_free(&b);
    return ok;
}

static void ignore(const char *name, int is_dir, uint64_t size, uint64_t mtime, void *context) {
    (void)name, (void)is_dir, (void)size, (void)mtime, (void)context;
}

/* Where a radio folder is: an absolute path as it is; a plain name in the music folder (<Azimuth>\EasyRadio\<name>),
   else in this mod's folder (the hand-installed layout). A name found in neither is created, empty, in the music
   folder, so the player sees where the MP3s go. NULL: no folder to read. */
static char *station_dir(const ErOptions *o, int station, const char *folder) {
    size_t n = er_strlen(folder);
    if (er_is_absolute(folder, n)) {
        while (n > 3 && (folder[n - 1] == '\\' || folder[n - 1] == '/')) --n;
        return er_strdup(folder, n);
    }
    char *music = o->music_dir ? path3(o->music_dir, folder, NULL) : NULL;
    if (music && er_list_dir(music, ignore, NULL)) return music;
    char *own = path3(o->mod_dir, folder, NULL);
    if (own && er_list_dir(own, ignore, NULL)) {
        er_free(music);
        return own;
    }
    er_free(own);
    if (music && er_make_dirs(music))
        er_log("WARN: %s: the folder %s did not exist; created it empty: put the station's MP3 files there", er_stations[station].id, music);
    else
        er_log("WARN: %s: cannot read the folder '%s'; the station stays stock", er_stations[station].id, folder);
    er_free(music);
    return NULL;
}

static void scan_station(const ErOptions *o, int station, const char *folder, Cache *cache, Cache *fresh, StationSongs *out, char **logo) {
    char *dir = station_dir(o, station, folder);
    Listing l = {0};
    if (!dir) return;
    if (!er_list_dir(dir, visit, &l)) {
        er_log("WARN: %s: cannot read the folder '%s'; the station stays stock", er_stations[station].id, dir);
        er_free(dir);
        return;
    }
    er_sort(l.items, l.count, sizeof(Item), item_less);
    out->songs = er_alloc((l.count ? l.count : 1) * sizeof(ErSong));
    unsigned skipped = 0, cached = 0;
    ErBuf artist = {0}, title = {0};
    char sep[2] = {o->separator, 0};
    for (size_t i = 0; out->songs && i < l.count && out->count < MAX_SONGS_PER_STATION; ++i) {
        Item *it = &l.items[i];
        char *file = path3(folder, "/", it->name);
        char *path = path3(dir, sep, it->name);
        if (!file || !path) {
            er_free(file);
            er_free(path);
            continue;
        }
        uint64_t ms = 0;
        const char *why = NULL;
        CacheEntry *hit = cache_find(cache, file, it->size, it->mtime);
        artist.length = title.length = 0;
        if (hit) {
            ++cached;
            ms = hit->ms;
            if (!ms) why = hit->title;
            er_buf_str(&artist, hit->artist);
            er_buf_str(&title, hit->title);
        } else {
            why = scan_song(path, it->name, &ms, &artist, &title);
        }
        if (cacheable(file)) cache_add(fresh, file, it->size, it->mtime, why ? 0 : ms, why ? "-" : er_buf_cstr(&artist), why ? why : er_buf_cstr(&title), 1);
        if (why) {
            ++skipped;
            er_log("SKIP %s: %s", file, why);
            er_free(file);
            er_free(path);
            continue;
        }
        ErSong *s = &out->songs[out->count++];
        s->station = station;
        s->number = er_stations[station].number;
        s->ms = ms;
        s->path = path;
        s->artist = er_strdup(er_buf_cstr(&artist), artist.length);
        s->title = er_strdup(er_buf_cstr(&title), title.length);
        er_free(file);
    }
    if (l.count > MAX_SONGS_PER_STATION) er_log("WARN: %s: only the first %u songs are used", folder, MAX_SONGS_PER_STATION);
    er_log("%s (%s) <- %s: %u songs, %u skipped, %u from the cache", er_stations[station].id, er_stations[station].name, dir,
           (unsigned)out->count, skipped, cached);
    if (l.logo) *logo = path3(dir, sep, l.logo);
    er_buf_free(&artist);
    er_buf_free(&title);
    listing_free(&l);
    er_free(dir);
}

/* ---- logo, cached by the image's hash ---- */
static uint64_t fnv1a(const void *data, size_t n) {
    const uint8_t *p = data;
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}
#define LOGO_ENCODER_VERSION 1
static int make_logo(const ErOptions *o, int station, const char *logo, const char *target) {
    ErBuf image = {0}, cached = {0}, name = {0};
    int ok = 0;
    if (!read_whole(logo, &image, 64ull * 1024 * 1024)) {
        er_log("WARN: %s: cannot read %s (or over 64 MiB); the stock icon stays", er_stations[station].id, logo);
        goto done;
    }
    uint64_t hash = fnv1a(image.data, image.length) ^ (uint64_t)LOGO_ENCODER_VERSION;
    er_buf_format(&name, "%slogo-%s-%llx.swatchbin", o->cache_dir, er_stations[station].icon, (unsigned long long)hash);
    if (read_whole(er_buf_cstr(&name), &cached, ER_SWATCH_SIZE) && cached.length == ER_SWATCH_SIZE) {
        ok = er_write_file(target, cached.data, cached.length);
        er_log("%s: HUD icon from the cache (%s)%s", er_stations[station].id, logo, ok ? "" : ": WRITE FAILED");
        goto done;
    }
    uint8_t *swatch = er_alloc(ER_SWATCH_SIZE);
    char why[96] = "out of memory";
    if (swatch && er_logo_swatch((const uint8_t *)image.data, image.length, swatch, why, sizeof(why))) {
        ok = er_write_file(target, swatch, ER_SWATCH_SIZE);
        if (ok) er_write_file(er_buf_cstr(&name), swatch, ER_SWATCH_SIZE);
        er_log("%s: HUD icon made from %s%s", er_stations[station].id, logo, ok ? "" : ": WRITE FAILED");
    } else {
        er_log("WARN: %s: %s: %s; the stock icon stays", er_stations[station].id, logo, why);
    }
    er_free(swatch);
done:
    er_buf_free(&image);
    er_buf_free(&cached);
    er_buf_free(&name);
    return ok;
}

/* ---- the whole run ---- */
void er_result_free(ErResult *r) {
    for (size_t i = 0; i < r->count; ++i) {
        er_free(r->songs[i].path);
        er_free(r->songs[i].artist);
        er_free(r->songs[i].title);
    }
    er_free(r->songs);
    er_config_free(&r->config);
    memset(r, 0, sizeof(*r));
}

static char *find_game_audio(const ErOptions *o, const ErConfig *config) {
    char sep[2] = {o->separator, 0};
    const char *mine[2] = {config->game_folder, NULL};
    for (int round = 0; round < 2; ++round) {
        const char *const *list = round ? o->game_dirs : mine;
        for (size_t i = 0; list && list[i]; ++i) {
            for (int mount = 0; mount < 2; ++mount) {
                ErBuf b = {0};
                er_buf_format(&b, "%s%s%s%smedia%saudio%s", list[i], sep, mount ? "Mount" : "", mount ? sep : "", sep, sep);
                char *probe = path3(er_buf_cstr(&b), "RadioInfo_EN.xml", NULL);
                ErFile *f = probe ? er_open(probe) : NULL;
                er_free(probe);
                if (f) {
                    er_close(f);
                    char *found = er_strdup(b.data, b.length);
                    er_buf_free(&b);
                    return found;
                }
                er_buf_free(&b);
            }
        }
    }
    return NULL;
}

int er_generate(const ErOptions *o, ErResult *r) {
    memset(r, 0, sizeof(*r));
    er_config_defaults(&r->config);
    char sep[2] = {o->separator, 0};
    char *ini_path = path3(o->mod_dir, "easyradio.ini", NULL);
    ErBuf ini = {0};
    int ini_ok = ini_path && read_whole(ini_path, &ini, 1024 * 1024);
    er_free(ini_path);
    /* the previous start's output goes first: a station no longer mapped must not keep its icon */
    for (int i = 0; i < ER_LANGUAGES; ++i) {
        ErBuf b = {0};
        er_buf_format(&b, "%saudio%sRadioInfo_%s.xml", o->out_dir, sep, er_languages[i]);
        er_delete_file(er_buf_cstr(&b));
        er_buf_free(&b);
    }
    for (int i = 0; i < ER_STATIONS; ++i) {
        ErBuf b = {0};
        er_buf_format(&b, "%sui%stextures%sAnthem%sHUD%sHUD_RadioIcon_%s.swatchbin", o->out_dir, sep, sep, sep, sep, er_stations[i].icon);
        er_delete_file(er_buf_cstr(&b));
        er_buf_free(&b);
    }
    if (!ini_ok) {
        er_log("FAIL: no easyradio.ini in the mod folder: the radio stays stock");
        return 0;
    }
    er_config_parse(&r->config, ini.data, ini.length);
    er_buf_free(&ini);
    int mapped = 0;
    for (int i = 0; i < ER_STATIONS; ++i) mapped += r->config.folder[i] != NULL;
    if (!mapped) {
        er_log("FAIL: easyradio.ini maps no station (every station stock; [radios] horizon=<folder>): the radio stays stock");
        return 0;
    }
    char *audio = find_game_audio(o, &r->config);
    if (!audio) {
        er_log("FAIL: the game's media\\audio\\RadioInfo_EN.xml was not found (set game_folder= in [options]): the radio stays stock");
        return 0;
    }
    er_log("game RadioInfo from %s", audio);
    er_make_dirs(o->cache_dir);
    char *cache_path = path3(o->cache_dir, "songs.tsv", NULL);
    Cache cache = {0}, fresh = {0};
    if (cache_path) cache_load(&cache, cache_path);

    StationSongs per[ER_STATIONS];
    char *logos[ER_STATIONS];
    memset(per, 0, sizeof(per));
    memset(logos, 0, sizeof(logos));
    ErRandom random = {o->seed};
    size_t total = 0;
    for (int i = 0; i < ER_STATIONS; ++i) {
        if (!r->config.folder[i]) continue;
        scan_station(o, i, r->config.folder[i], &cache, &fresh, &per[i], &logos[i]);
        if (!per[i].count) {
            er_log("WARN: %s: no playable .mp3 in '%s'; the station stays stock", er_stations[i].id, r->config.folder[i]);
            continue;
        }
        if (o->shuffle) er_shuffle(per[i].songs, per[i].count, sizeof(ErSong), &random);
        for (size_t k = 0; k < per[i].count; ++k) song_key(per[i].songs[k].key, sizeof(per[i].songs[k].key), i, k + 1);
        r->station_songs[i] = (unsigned)per[i].count;
        total += per[i].count;
    }
    if (cache_path && fresh.count) {
        ErBuf out = {0};
        er_buf_str(&out, CACHE_MAGIC "\n");
        for (size_t i = 0; i < fresh.count; ++i)
            er_buf_format(&out, "%s\t%llu\t%llu\t%llu\t%s\t%s\n", fresh.entries[i].file, (unsigned long long)fresh.entries[i].size,
                          (unsigned long long)fresh.entries[i].mtime, (unsigned long long)fresh.entries[i].ms, fresh.entries[i].artist,
                          fresh.entries[i].title);
        if (!out.failed) er_write_file(cache_path, out.data, out.length);
        er_buf_free(&out);
    }
    cache_free(&cache);
    cache_free(&fresh);
    er_free(cache_path);

    if (total) {
        r->songs = er_alloc(total * sizeof(ErSong));
        for (int i = 0; r->songs && i < ER_STATIONS; ++i) {
            if (per[i].count) memcpy(r->songs + r->count, per[i].songs, per[i].count * sizeof(ErSong));
            r->count += per[i].songs ? per[i].count : 0;
        }
        if (!r->songs) total = 0;
    }
    for (int i = 0; i < ER_STATIONS; ++i) {
        if (!r->songs)
            for (size_t k = 0; k < per[i].count; ++k) {
                er_free(per[i].songs[k].path);
                er_free(per[i].songs[k].artist);
                er_free(per[i].songs[k].title);
            }
        er_free(per[i].songs);
    }
    if (!total) {
        for (int i = 0; i < ER_STATIONS; ++i) er_free(logos[i]);
        er_free(audio);
        er_log("FAIL: no station got a playable song: the radio stays stock");
        r->count = 0;
        return 0;
    }

    /* RadioInfo for every language the game has */
    char *audio_out = NULL;
    {
        ErBuf b = {0};
        er_buf_format(&b, "%saudio%s", o->out_dir, sep);
        audio_out = er_strdup(b.data ? b.data : "", b.length);
        er_buf_free(&b);
    }
    er_make_dirs(audio_out);
    ErEntry *entries = er_alloc(total * sizeof(ErEntry));
    for (size_t k = 0; entries && k < r->count; ++k) {
        memcpy(entries[k].key, r->songs[k].key, sizeof(entries[k].key));
        entries[k].artist = r->songs[k].artist;
        entries[k].title = r->songs[k].title;
        entries[k].ms = r->songs[k].ms;
    }
    for (int lang = 0; entries && lang < ER_LANGUAGES; ++lang) {
        ErBuf name = {0}, text = {0}, edited = {0};
        er_buf_format(&name, "RadioInfo_%s.xml", er_languages[lang]);
        char *source = path3(audio, er_buf_cstr(&name), NULL);
        if (!source || !read_whole(source, &text, 16 * 1024 * 1024)) {
            er_log("WARN: cannot read the game's %s; that language stays stock", er_buf_cstr(&name));
        } else {
            int edits = 0;
            for (int i = 0; i < ER_STATIONS; ++i) {
                if (!r->station_songs[i]) {
                    continue;
                }
                size_t first = 0;
                for (size_t k = 0; k < r->count; ++k)
                    if (r->songs[k].station == i) {
                        first = k;
                        break;
                    }
                edited.length = 0;
                if (er_edit_station(text.data, text.length, er_stations[i].name, entries + first, r->station_songs[i], r->config.dj_off, &edited)) {
                    ErBuf swap = text;
                    text = edited;
                    edited = swap;
                    ++edits;
                } else {
                    er_log("WARN: %s: %s keeps its stock songs in this language", er_buf_cstr(&name), er_stations[i].name);
                }
            }
            if (edits) {
                char *target = path3(audio_out, er_buf_cstr(&name), NULL);
                if (target && er_write_file(target, text.data, text.length)) ++r->languages;
                else er_log("WARN: cannot write %s", target ? target : "RadioInfo");
                er_free(target);
            }
        }
        er_free(source);
        er_buf_free(&name);
        er_buf_free(&text);
        er_buf_free(&edited);
    }
    er_free(entries);
    er_free(audio_out);
    er_free(audio);

    if (!r->languages) {
        for (int i = 0; i < ER_STATIONS; ++i) er_free(logos[i]);
        er_log("FAIL: no RadioInfo file could be written: the radio stays stock");
        return 0;
    }
    /* HUD icons */
    for (int i = 0; i < ER_STATIONS; ++i) {
        if (!r->station_songs[i]) {
            er_free(logos[i]);
            continue;
        }
        if (!logos[i]) {
            er_log("%s: no logo.png in '%s'; the stock icon stays", er_stations[i].id, r->config.folder[i]);
            continue;
        }
        ErBuf dir = {0}, target = {0};
        er_buf_format(&dir, "%sui%stextures%sAnthem%sHUD%s", o->out_dir, sep, sep, sep, sep);
        er_make_dirs(er_buf_cstr(&dir));
        er_buf_format(&target, "%sHUD_RadioIcon_%s.swatchbin", er_buf_cstr(&dir), er_stations[i].icon);
        if (make_logo(o, i, logos[i], er_buf_cstr(&target))) ++r->logos;
        er_buf_free(&dir);
        er_buf_free(&target);
        er_free(logos[i]);
    }
    return 1;
}
