/* EasyRadio generator: easyradio.ini + radio folders + the game's RadioInfo_<lang>.xml -> the files the game reads
   (RadioInfo for all languages, HUD icons) in a private media root, and the song table the hooks answer from. */
#pragma once
#include "er_core.h"

#define ER_KEY_PREFIX "EASYRADIO_"
#define ER_MIN_SONG_MS 10000u
#define ER_LANGUAGES 10
extern const char *const er_languages[ER_LANGUAGES];

typedef struct {
    char key[40];         /* EASYRADIO_<station id>_<n>, n = position in this start's shuffled order */
    int station;          /* index into er_stations */
    int number;           /* RadioStation Number */
    uint64_t ms;          /* the XML's SampleLength and End */
    char *path;           /* UTF-8 absolute path */
    char *artist, *title; /* ASCII HUD text */
} ErSong;

typedef struct {
    ErSong *songs;
    size_t count;
    unsigned station_songs[ER_STATIONS]; /* 0: the station stays stock */
    unsigned languages;                  /* RadioInfo files written */
    unsigned logos;                      /* HUD icons written */
    ErConfig config;
} ErResult;

typedef struct {
    const char *mod_dir;     /* the mod folder, ending in a separator */
    const char *out_dir;     /* private media root (as the game's media\), ending in a separator; rebuilt */
    const char *cache_dir;   /* ending in a separator */
    const char *const *game_dirs; /* candidate game content folders (holding media\audio), NULL-terminated */
    char separator;          /* '\\' or '/' */
    int shuffle;             /* 0: file-name order (for comparisons with the Python prototype) */
    uint64_t seed;
    const char *music_dir;   /* the player's radio folders (<Azimuth>\\EasyRadio\\), ending in a separator; NULL: only the mod's */
} ErOptions;

/* 1 when at least one station got songs (then the XML for every readable language was written); 0 with every
   problem logged, nothing usable written, and the radio left stock. */
int er_generate(const ErOptions *options, ErResult *result);
void er_result_free(ErResult *result);

/* ---- playing sounds (er_track.c): each FMOD sound tied to the song its own createSound named ---- */
#define ER_TRACKED 32
#define ER_SEEN 4
typedef struct {
    void *sound;          /* FMOD Sound; NULL: free */
    const ErSong *song;   /* the song its createSound named */
    uint64_t generation;  /* createSound order */
    unsigned length_ms;   /* FMOD's own length (getSubSound), 0 until measured */
    int state, result;    /* last getOpenState, -1 none */
    int announced;        /* "playing" was logged for it */
} ErTrack;
typedef struct {
    void *stream;         /* RadioStreamFmod; NULL: free */
    void *sound;
    const ErSong *song;
    unsigned length_ms;
    int position;         /* its timeline position, ms */
    uint64_t tick;        /* when the radio update last saw it */
} ErSeen;
typedef struct {
    ErTrack track[ER_TRACKED];
    ErSeen seen[ER_SEEN];
    uint64_t generation;
} ErTracker;
/* every createSound: forgets what this address meant before; song NULL (not ours) stops there */
void er_track_created(ErTracker *t, void *sound, const ErSong *song);
ErTrack *er_track_find(ErTracker *t, const void *sound);
/* the radio update saw stream play sound at position: the song, or NULL when the sound is not ours (the stream's
   record is dropped then). *first: 1 the first time this sound is seen playing (log it once). */
const ErSong *er_track_seen(ErTracker *t, void *stream, void *sound, int position, uint64_t now, int *first);
/* the stream of station `number` seen at most max_age ms ago whose position is closest to the station clock */
int er_track_current(const ErTracker *t, int number, int clock, uint64_t now, uint64_t max_age, ErSeen *out);
/* the newest tick any stream of ours was seen at (0: none) and its song */
uint64_t er_track_last_tick(const ErTracker *t, const ErSong **song);
/* the skip position: 1.5 s before the song's end, the earlier of the XML End and FMOD's own length */
int er_skip_target(const ErSong *song, unsigned fmod_length_ms);
