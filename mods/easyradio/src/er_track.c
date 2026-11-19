/* EasyRadio core: which FMOD sound plays which of our songs, and which radio stream plays it now.

   FMOD reuses a released sound's address for the next sound it creates (a skip ends a song, the next one is created
   at once). So a sound is tied to the song its own createSound named, and every createSound, ours or not, first
   forgets whatever that address meant before: an old entry can never answer for a new sound. Around a track change
   two RadioStreamFmod objects update at the same time (the outgoing one and the new one); each stream keeps its own
   record, and the skip picks the one whose position matches the station's clock.

   Not thread-safe by itself: the plugin calls these under one lock (createSound may run on FMOD's loader thread,
   the stream updates on the radio thread). No allocation, no logging. */
#include "er_gen.h"

void er_track_created(ErTracker *t, void *sound, const ErSong *song) {
    if (!sound) return;
    for (int i = 0; i < ER_TRACKED; ++i)
        if (t->track[i].sound == sound) t->track[i] = (ErTrack){0};
    /* a stream whose sound address was reused no longer plays what it played */
    for (int i = 0; i < ER_SEEN; ++i)
        if (t->seen[i].sound == sound) t->seen[i] = (ErSeen){0};
    if (!song) return;
    int slot = 0;
    for (int i = 0; i < ER_TRACKED; ++i) {
        if (!t->track[i].sound) {
            slot = i;
            break;
        }
        if (t->track[i].generation < t->track[slot].generation) slot = i;
    }
    t->track[slot] = (ErTrack){sound, song, ++t->generation, 0, -1, -1, 0};
}

ErTrack *er_track_find(ErTracker *t, const void *sound) {
    if (!sound) return 0;
    for (int i = 0; i < ER_TRACKED; ++i)
        if (t->track[i].sound == sound) return &t->track[i];
    return 0;
}

const ErSong *er_track_seen(ErTracker *t, void *stream, void *sound, int position, uint64_t now, int *first) {
    if (first) *first = 0;
    if (!stream) return 0;
    ErTrack *track = er_track_find(t, sound);
    int slot = -1;
    for (int i = 0; i < ER_SEEN; ++i)
        if (t->seen[i].stream == stream) slot = i;
    if (!track) {
        if (slot >= 0) t->seen[slot] = (ErSeen){0}; /* this stream plays something else now */
        return 0;
    }
    if (slot < 0) {
        slot = 0;
        for (int i = 0; i < ER_SEEN; ++i) {
            if (!t->seen[i].stream) {
                slot = i;
                break;
            }
            if (t->seen[i].tick < t->seen[slot].tick) slot = i;
        }
    }
    t->seen[slot] = (ErSeen){stream, sound, track->song, track->length_ms, position, now};
    if (!track->announced) {
        track->announced = 1;
        if (first) *first = 1;
    }
    return track->song;
}

int er_track_current(const ErTracker *t, int number, int clock, uint64_t now, uint64_t max_age, ErSeen *out) {
    int best = -1;
    long long best_gap = 0;
    for (int i = 0; i < ER_SEEN; ++i) {
        const ErSeen *s = &t->seen[i];
        if (!s->stream || !s->song || s->song->number != number || now - s->tick > max_age) continue;
        long long gap = (long long)clock - s->position;
        if (gap < 0) gap = -gap;
        if (best < 0 || gap < best_gap || (gap == best_gap && s->tick > t->seen[best].tick)) best = i, best_gap = gap;
    }
    if (best < 0) return 0;
    *out = t->seen[best];
    return 1;
}

uint64_t er_track_last_tick(const ErTracker *t, const ErSong **song) {
    uint64_t tick = 0;
    if (song) *song = 0;
    for (int i = 0; i < ER_SEEN; ++i)
        if (t->seen[i].stream && t->seen[i].tick >= tick) {
            tick = t->seen[i].tick;
            if (song) *song = t->seen[i].song;
        }
    return tick;
}

int er_skip_target(const ErSong *song, unsigned fmod_length_ms) {
    uint64_t end = song->ms;
    if (fmod_length_ms && fmod_length_ms < end) end = fmod_length_ms; /* never past the file's own end */
    return end > 1500 ? (int)(end - 1500) : 0;
}
