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
