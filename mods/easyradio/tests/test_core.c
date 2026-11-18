/* EasyRadio host unit tests: the plugin's core C (ini, ID3, MP3 duration incl. VBR, HUD text, XML, shuffle, logo,
   generator) compiled for Linux. Synthetic data only. Build and run: python3 -I mods/easyradio/tests/run_tests.py --zig ZIG */
#define _GNU_SOURCE
#include "../src/er_gen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern int er_test_quiet;
static int checks, failed;
#define CHECK(cond, ...)                                                                                                                   \
    do {                                                                                                                                   \
        ++checks;                                                                                                                          \
        if (!(cond)) {                                                                                                                     \
            ++failed;                                                                                                                      \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                                  \
            printf(__VA_ARGS__);                                                                                                           \
            printf("\n");                                                                                                                  \
        }                                                                                                                                  \
    } while (0)

/* ---- byte builders ---- */
typedef struct {
    unsigned char *d;
    size_t n, cap;
} Bytes;
static void add(Bytes *b, const void *p, size_t n) {
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 64;
        b->d = realloc(b->d, b->cap);
    }
    memcpy(b->d + b->n, p, n);
    b->n += n;
}
static void add_zero(Bytes *b, size_t n) {
    unsigned char z[4096] = {0};
    while (n) {
        size_t k = n < sizeof(z) ? n : sizeof(z);
        add(b, z, k);
        n -= k;
    }
}
static void be32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v >> 24), p[1] = (unsigned char)(v >> 16), p[2] = (unsigned char)(v >> 8), p[3] = (unsigned char)v;
}
static void syncsafe(unsigned char *p, uint32_t v) {
    p[0] = (v >> 21) & 127, p[1] = (v >> 14) & 127, p[2] = (v >> 7) & 127, p[3] = v & 127;
}

/* MPEG1 Layer III, 128 kbps, 44.1 kHz, stereo: 417-byte frames of 1152 samples (as test_easyradio.py) */
static const unsigned char HEADER[4] = {0xff, 0xfb, 0x90, 0x00};
static void frames(Bytes *b, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        add(b, HEADER, 4);
        add_zero(b, 413);
    }
}
static void xing_frame(Bytes *b, uint32_t count, const char *magic) {
    unsigned char f[417] = {0};
    memcpy(f, HEADER, 4);
    memcpy(f + 4 + 32, magic, 4);
    be32(f + 4 + 32 + 4, 1);
    be32(f + 4 + 32 + 8, count);
    add(b, f, sizeof(f));
}
static uint64_t ms_of(uint64_t frames_count) {
    return frames_count * 1152 * 1000 / 44100;
}

/* ID3v2 tag with TIT2/TPE1 in a chosen encoding; text is UTF-8 */
static void text_frame(Bytes *frames_out, const char *id, int major, int encoding, const char *text) {
    Bytes body = {0};
    unsigned char e = (unsigned char)encoding;
    add(&body, &e, 1);
    if (encoding == 1 || encoding == 2) { /* UTF-16 (LE with BOM) / BE: ASCII + a few Latin-1 */
        if (encoding == 1) add(&body, "\xff\xfe", 2);
        for (const unsigned char *s = (const unsigned char *)text; *s;) {
            uint32_t cp = *s++;
            if (cp >= 0xc0) {
                cp = ((cp & 0x1f) << 6) | (*s++ & 0x3f);
            }
            unsigned char u[2] = {(unsigned char)(encoding == 1 ? cp : cp >> 8), (unsigned char)(encoding == 1 ? cp >> 8 : cp)};
            add(&body, u, 2);
        }
        add(&body, "\0\0", 2);
    } else {
        add(&body, text, strlen(text));
    }
    unsigned char head[10] = {0};
    size_t head_size = major == 2 ? 6 : 10;
    memcpy(head, id, major == 2 ? 3 : 4);
    if (major == 2) head[3] = (unsigned char)(body.n >> 16), head[4] = (unsigned char)(body.n >> 8), head[5] = (unsigned char)body.n;
    else if (major == 4) syncsafe(head + 4, (uint32_t)body.n);
    else be32(head + 4, (uint32_t)body.n);
    add(frames_out, head, head_size);
    add(frames_out, body.d, body.n);
    free(body.d);
}
static void id3(Bytes *b, int major, int encoding, const char *title, const char *artist, size_t padding) {
    Bytes f = {0};
    if (title) text_frame(&f, major == 2 ? "TT2" : "TIT2", major, encoding, title);
    if (artist) text_frame(&f, major == 2 ? "TP1" : "TPE1", major, encoding, artist);
    add_zero(&f, padding);
    unsigned char head[10] = {'I', 'D', '3', (unsigned char)major, 0, 0};
    syncsafe(head + 6, (uint32_t)f.n);
    add(b, head, 10);
    add(b, f.d, f.n);
    free(f.d);
}

static void text_of(ErText *t, const char *utf8) {
    t->n = 0;
    er_text_from_utf8_lossy(t, utf8, strlen(utf8));
}
static char *utf8_of(const ErText *t) {
    ErBuf b = {0};
    er_text_to_utf8(t, &b);
    char *s = strdup(er_buf_cstr(&b));
    er_buf_free(&b);
    return s;
}

/* ---- duration ---- */
static void test_duration(void) {
    uint64_t ms = 0;
    Bytes b = {0};
    id3(&b, 3, 3, "T", "A", 0);
    frames(&b, 1000);
    CHECK(er_mp3_duration_ms(b.d, b.n, &ms) == 1 && ms == ms_of(1000), "CBR frame count: %llu", (unsigned long long)ms);
    CHECK(er_mp3_duration_prefix(b.d, 64 * 1024, b.n, &ms) == -1, "a CBR file without Xing needs the whole file");
    free(b.d);

    memset(&b, 0, sizeof(b));
    xing_frame(&b, 5000, "Xing");
    frames(&b, 10);
    CHECK(er_mp3_duration_ms(b.d, b.n, &ms) == 1 && ms == ms_of(5000), "Xing frame count wins: %llu", (unsigned long long)ms);
    free(b.d);

    /* VBR, 11 minutes (long songs must not be cut): Xing from a 64 KiB prefix of a 30 MB file */
    memset(&b, 0, sizeof(b));
    id3(&b, 4, 3, "Long", "Artist", 0);
    xing_frame(&b, 25000, "Info");
    frames(&b, 200);
    uint64_t total = 30u * 1024 * 1024;
    CHECK(er_mp3_duration_prefix(b.d, b.n, total, &ms) == 1 && ms == ms_of(25000) && ms > 600000, "Info frames from a prefix: %llu ms",
          (unsigned long long)ms);
    free(b.d);

    /* VBRI */
    memset(&b, 0, sizeof(b));
    {
        unsigned char f[417] = {0};
        memcpy(f, HEADER, 4);
        memcpy(f + 36, "VBRI", 4);
        be32(f + 36 + 14, 777);
        add(&b, f, sizeof(f));
        frames(&b, 3);
    }
    CHECK(er_mp3_duration_ms(b.d, b.n, &ms) == 1 && ms == ms_of(777), "VBRI frame count: %llu", (unsigned long long)ms);
    free(b.d);

    /* junk before the frames and an ID3v1 tag: 11.5 minutes of CBR by frame scan */
    memset(&b, 0, sizeof(b));
    add_zero(&b, 37);
    frames(&b, 26500);
    add(&b, "TAG", 3);
    add_zero(&b, 125);
    CHECK(er_mp3_duration_ms(b.d, b.n, &ms) == 1 && ms == ms_of(26500) && ms > 600000, "junk + ID3v1, 11.5 min: %llu", (unsigned long long)ms);
    free(b.d);

    /* nothing */
    unsigned char junk[5000] = {0};
    CHECK(er_mp3_duration_ms(junk, sizeof(junk), &ms) == 0, "no frames");
    CHECK(er_mp3_duration_ms(junk, 3, &ms) == 0, "tiny file");

    /* a tag larger than the prefix: the prefix cannot answer */
    memset(&b, 0, sizeof(b));
    id3(&b, 3, 3, "T", "A", 700000);
    xing_frame(&b, 100, "Xing");
    frames(&b, 3);
    CHECK(er_mp3_duration_prefix(b.d, 512 * 1024, b.n + 10000, &ms) == -1, "tag past the prefix");
    CHECK(er_mp3_duration_ms(b.d, b.n, &ms) == 1 && ms == ms_of(100), "big tag, whole file: %llu", (unsigned long long)ms);
    free(b.d);

    /* MPEG2 Layer III 64 kbps 22.05 kHz mono: 576-sample frames of 208 bytes */
    memset(&b, 0, sizeof(b));
    for (int i = 0; i < 100; ++i) {
        unsigned char h[4] = {0xff, 0xf3, 0x80, 0xc0};
        add(&b, h, 4);
        add_zero(&b, 72 * 64000 / 22050 - 4);
    }
    CHECK(er_mp3_duration_ms(b.d, b.n, &ms) == 1 && ms == 100ull * 576 * 1000 / 22050, "MPEG2 frames: %llu", (unsigned long long)ms);
    free(b.d);
}

/* ---- tags ---- */
static void tags_case(int major, int encoding, const char *title, const char *artist, const char *want_title, const char *want_artist) {
    Bytes b = {0};
    id3(&b, major, encoding, title, artist, 16);
    frames(&b, 2);
    ErTags t = {{0}, {0}};
    er_id3v2_text(b.d, b.n, &t);
    char *got_t = utf8_of(&t.title), *got_a = utf8_of(&t.artist);
    CHECK(!strcmp(got_t, want_title) && !strcmp(got_a, want_artist), "ID3v2.%d enc %d: '%s' / '%s'", major, encoding, got_t, got_a);
    free(got_t);
    free(got_a);
    er_tags_free(&t);
    free(b.d);
}
static void test_tags(void) {
    tags_case(3, 3, "Título", "Ärtist", "Título", "Ärtist");
    tags_case(3, 1, "Odd", "Len", "Odd", "Len");
    tags_case(4, 1, "Señor", "Café", "Señor", "Café");
    tags_case(4, 2, "Big", "Endian", "Big", "Endian");
    tags_case(4, 0, "Latin", "One", "Latin", "One");
    tags_case(2, 3, "Old", "Tag", "Old", "Tag");
    tags_case(3, 3, "  spaced  ", "x\xc2\xa0", "spaced", "x"); /* strip() includes U+00A0 */

    /* broken UTF-16 (odd length) is empty, like Python's strict decoder */
    Bytes b = {0};
    {
        Bytes f = {0};
        unsigned char head[10] = {'T', 'I', 'T', '2'};
        unsigned char body[4] = {1, 0xff, 0xfe, 'A'};
        be32(head + 4, 4);
        add(&f, head, 10);
        add(&f, body, 4);
        unsigned char h[10] = {'I', 'D', '3', 3, 0, 0};
        syncsafe(h + 6, (uint32_t)f.n);
        add(&b, h, 10);
        add(&b, f.d, f.n);
        free(f.d);
    }
    ErTags t = {{0}, {0}};
    er_id3v2_text(b.d, b.n, &t);
    CHECK(t.title.n == 0, "odd-length UTF-16 is dropped");
    /* ID3v1 behind it fills the gap; ID3v2 wins where both exist */
    frames(&b, 2);
    unsigned char v1[128] = {'T', 'A', 'G'};
    memcpy(v1 + 3, "V1 Title", 8);
    memcpy(v1 + 33, "V1 Artist", 9);
    add(&b, v1, 128);
    er_tags_free(&t);
    er_id3v1_text(b.d, b.n, &t);
    er_id3v2_text(b.d, b.n, &t);
    char *got = utf8_of(&t.title);
    CHECK(!strcmp(got, "V1 Title"), "ID3v1 title fallback: %s", got);
    free(got);
    er_tags_free(&t);
    free(b.d);

    /* unsynchronisation flag: no tags (as the prototype) */
    memset(&b, 0, sizeof(b));
    id3(&b, 3, 3, "T", "A", 0);
    b.d[5] = 0x80;
    er_id3v2_text(b.d, b.n, &t);
    CHECK(!t.title.n && !t.artist.n, "unsynchronised tag ignored");
    er_tags_free(&t);
    free(b.d);
}

/* ---- HUD text (the Python prototype's cases, plus more) ---- */
static void names_case(const char *stem, const char *title, const char *artist, const char *want_artist, const char *want_title) {
    ErText s = {0}, t = {0}, a = {0};
    text_of(&s, stem);
    if (title) text_of(&t, title);
    if (artist) text_of(&a, artist);
    ErBuf ga = {0}, gt = {0};
    er_hud_names(&s, &t, &a, &ga, &gt);
    CHECK(!strcmp(er_buf_cstr(&ga), want_artist) && !strcmp(er_buf_cstr(&gt), want_title), "names('%s','%s','%s') = ('%s','%s'), want ('%s','%s')", stem,
          title ? title : "", artist ? artist : "", er_buf_cstr(&ga), er_buf_cstr(&gt), want_artist, want_title);
    er_buf_free(&ga);
    er_buf_free(&gt);
    er_text_free(&s);
    er_text_free(&t);
    er_text_free(&a);
}
static void test_names(void) {
    names_case("x", "Shakira - Hips Don't Lie (featuring Wyclef Jean) (Official 4K Video) ft. Wyclef Jean", "Shakira", "Shakira",
               "Hips Don't Lie ft. Wyclef Jean");
    names_case("x", "Gnarls Barkley - Crazy (Official Video) [4K Remaster]", "GB Official", "Gnarls Barkley", "Crazy");
    names_case("x", "P. Diddy [feat. Nelly & Murphy Lee] - Shake Ya Tailfeather (Official Music Video)", "Bad Boy", "P. Diddy",
               "Shake Ya Tailfeather ft. Nelly & Murphy Lee");
    names_case("x", "Locked Up", "Akon", "Akon", "Locked Up");
    names_case("x", "Fun - Song", "ChamillionaireVEVO", "Fun", "Song");
    names_case("x", "JA\xc5\xb8-Z - Change Clothes ft. Pharrell", "JA\xc5\xb8-Z", "JAY-Z", "Change Clothes ft. Pharrell");
    names_case("x", "Beyonc\xc3\xa9 - Baby Boy", NULL, "Beyonce", "Baby Boy");
    names_case("x", "98\xc2\xba - Give Me Just One Night (Una Noche) (Official Music Video)", "98 Degrees", "98 Degrees",
               "Give Me Just One Night (Una Noche)");
    names_case("x", "112 - Peaches And Cream (Official Music Video)", "Bad Boy Entertainment", "112", "Peaches And Cream");
    names_case("M\xc3\xbd" "a - Mya - Case Of The Ex (Whatcha Gonna Do) (Official Music Video)", NULL, NULL, "Mya", "Case Of The Ex (Whatcha Gonna Do)");
    names_case("Artist - Title [Official Video]", NULL, NULL, "Artist", "Title");
    names_case("Just A Title", NULL, "Tag Artist", "Tag Artist", "Just A Title");
    names_case("x", "A - Song EXPLICIT VERSION", NULL, "A", "Song");
    names_case("x", "A - Song (Lyrics)", NULL, "A", "Song");
    names_case("x", "A - Song (Lyric Video)", NULL, "A", "Song");
    names_case("x", "A - Song [HD]", NULL, "A", "Song");
    names_case("x", "A - Shadow (Visualizer)", NULL, "A", "Shadow");
    names_case("x", "A - Hd Days (Live)", NULL, "A", "Hd Days (Live)");
    names_case("x", "\xe2\x80\x9cQuoted\xe2\x80\x9d \xe2\x80\x94 x - It\xe2\x80\x99s <here>", NULL, "'Quoted' x", "It's here");
    names_case("x", "\xef\xbc\xa6\xef\xbd\x95\xef\xbd\x8c\xef\xbd\x8c - W\xc3\xa4rme", NULL, "Full", "Warme");
    names_case("x", "\xe6\x97\xa5\xe6\x9c\xac - \xe6\x9b\xb2", NULL, "Unknown Artist", "x");
    names_case("\xe6\x9b\xb2", NULL, NULL, "Unknown Artist", "Untitled");
    names_case("x", "A ft. B - Song", NULL, "A", "Song ft. B");
    names_case("x", "A feat. B - Song ft. B", NULL, "A", "Song ft. B");
    names_case("x", "Defeat - Song", NULL, "Defeat", "Song");
    /* ellipsis: 80 title characters, 40 artist characters */
    {
        char title[400] = "A - ";
        for (int i = 0; i < 30; ++i) strcat(title, "word ");
        ErText s = {0}, t = {0}, a = {0};
        text_of(&s, "x");
        text_of(&t, title);
        ErBuf ga = {0}, gt = {0};
        er_hud_names(&s, &t, &a, &ga, &gt);
        CHECK(gt.length == 80 && !strcmp(gt.data + 77, "..."), "title ellipsis (%u)", (unsigned)gt.length);
        er_buf_free(&ga);
        er_buf_free(&gt);
        text_of(&t, "Somebody With A Very Long Artist Name Indeed Yes - T");
        er_hud_names(&s, &t, &a, &ga, &gt);
        CHECK(ga.length <= 40 && !strcmp(ga.data + ga.length - 3, "..."), "artist ellipsis '%s'", er_buf_cstr(&ga));
        er_buf_free(&ga);
        er_buf_free(&gt);
        er_text_free(&s);
        er_text_free(&t);
    }
    /* every output byte is printable ASCII without < > " */
    {
        ErText t = {0};
        for (uint32_t cp = 1; cp < 0x3000; ++cp) er_text_push(&t, cp);
        ErBuf out = {0};
        er_ascii_text(&t, &out);
        int ascii = 1;
        for (size_t i = 0; i < out.length; ++i)
            if (out.data[i] < ' ' || out.data[i] > '~' || out.data[i] == '<' || out.data[i] == '>' || out.data[i] == '"') ascii = 0;
        CHECK(ascii && out.length > 1000, "ascii_text output is HUD-safe ASCII (%u bytes)", (unsigned)out.length);
        er_buf_free(&out);
        er_text_free(&t);
    }
}

/* ---- ini ---- */
static void test_ini(void) {
    ErConfig c;
    er_test_quiet = 1;
    er_config_defaults(&c);
    const char *good = "\xef\xbb\xbf; comment\n[radios]\nhorizon = nfsu-radio ; the folder\nbass=My-Folder#2\n[options]\ndj=off\nskip_key=F8\nskip_pad=off\n";
    er_config_parse(&c, good, strlen(good));
    CHECK(c.folder[0] && !strcmp(c.folder[0], "nfsu-radio"), "horizon alias of pulse: %s", c.folder[0] ? c.folder[0] : "(none)");
    CHECK(c.folder[1] && !strcmp(c.folder[1], "My-Folder#2"), "'#' without a blank before it is part of the value: %s", c.folder[1] ? c.folder[1] : "");
    CHECK(c.dj_off == 1 && c.skip_vk == 0x77 && c.skip_button == 0 && c.errors == 0, "options dj=off F8 pad off (errors %d)", c.errors);
    er_config_free(&c);

    const char *bad = "[radios]\nfoo=party\npulse=../up\nxs=\nlevante=ok\n[options]\ndj=maybe\nskip_key=F13\nskip_pad=Z\nwhat=1\n[other]\nx=1\n";
    er_config_parse(&c, bad, strlen(bad));
    CHECK(c.errors == 9 && !c.folder[0] && !c.folder[2] && c.folder[3], "bad lines logged and ignored (%d)", c.errors);
    CHECK(c.skip_vk == 0x75 && c.skip_button == 0x40 && c.dj_off == 0, "defaults stay on bad options");
    er_config_free(&c);

    /* the launcher's form: every station written, "stock" for the ones that keep their music; absolute paths */
    const char *form = "[radios]\nhorizon=music1\nbass=Stock\nxs=C:\\Users\\me\\Music\\x\nlevante=\\\\nas\\share\\y\nhospital=D:/z/\n"
                       "ninjatune=stock\ninnovative=/home/me/w\n[options]\ndj=keep\nskip_key=F6\nskip_pad=LS\n";
    er_config_parse(&c, form, strlen(form));
    CHECK(c.errors == 0 && c.folder[0] && !c.folder[1] && !c.folder[5], "stock keeps the station (errors %d)", c.errors);
    CHECK(c.folder[2] && !strcmp(c.folder[2], "C:\\Users\\me\\Music\\x") && c.folder[3] && !strcmp(c.folder[3], "\\\\nas\\share\\y") &&
              c.folder[4] && !strcmp(c.folder[4], "D:/z/") && c.folder[6] && !strcmp(c.folder[6], "/home/me/w"),
          "absolute folders kept as written");
    er_config_free(&c);
    const char *later = "[radios]\npulse=a\npulse=stock\nbass=a/b\nxs=C:x\nlevante=..\nhospital=x:y\nninjatune=C:\\a*b\n";
    er_config_parse(&c, later, strlen(later));
    CHECK(!c.folder[0] && c.errors == 5, "a later stock line unmaps; relative paths, C:x, .., x:y and wildcards refused (%d)", c.errors);
    er_config_free(&c);

    const char *several = "[radios]\npulse=a\nbass=b\nninjatune=c\nINNOVATIVE=d\nhorizon=e\n";
    er_config_parse(&c, several, strlen(several));
    CHECK(c.folder[0] && !strcmp(c.folder[0], "e") && c.folder[1] && c.folder[5] && c.folder[6], "several stations; a later alias wins");
    er_config_free(&c);
    er_test_quiet = 0;
}

/* ---- XML ---- */
static const char *SAMPLE =
    "<Radio>\r\n  <RadioStations>\r\n    <RadioStation Name=\"Horizon Pulse\" Number=\"1\" RecentlyPlayedListSize=\"-1\">\r\n"
    "      <Playlist Type=\"FreeroamTracks\">\r\n        <Entry EventName=\"/Master/Radio/Track\" SoundName=\"A&B\" SampleLength=\"1\" "
    "DisplayName=\"x & y\" Artist=\"z\" TrackStart=\"0\" DJDrop=\"0\" TrackDrop=\"0\" TrackBreakDown=\"0\" StingerStart=\"0\" DJStart=\"0\" "
    "End=\"1\"/>\r\n      </Playlist>\r\n      <Playlist Type=\"EventTracks\">\r\n        <Entry SoundName=\"B\"/>\r\n      </Playlist>\r\n"
    "      <Playlist Type=\"Stingers\">\r\n        <Entry EventName=\"/Master/Radio/Stinger\" SoundName=\"S\" StartNextTrack=\"1\" End=\"1\"/>\r\n"
    "      </Playlist>\r\n      <Playlist Type=\"DJ\">\r\n        <Entry SoundName=\"D\" GameEvent=\"G\" End=\"1\"/>\r\n      </Playlist>\r\n"
    "    </RadioStation>\r\n    <RadioStation Name=\"Bass Arena\" Number=\"2\">\r\n      <Playlist Type=\"FreeroamTracks\">\r\n"
    "        <Entry SoundName=\"K2\"/>\r\n      </Playlist>\r\n    </RadioStation>\r\n  </RadioStations>\r\n</Radio>\r\n";

static void test_xml(void) {
    ErEntry e[2] = {{"EASYRADIO_pulse_1", "A", "One & Two", 185051}, {"EASYRADIO_pulse_2", "B", "Long", 660035}};
    ErBuf out = {0};
    CHECK(er_edit_station(SAMPLE, strlen(SAMPLE), "Horizon Pulse", e, 2, 1, &out) == 1, "edit ok");
    const char *s = er_buf_cstr(&out);
    CHECK(strstr(s, "SoundName=\"K2\"") != NULL, "other stations stay");
    CHECK(!strstr(s, "SoundName=\"S\"") && !strstr(s, "SoundName=\"D\""), "dj=off empties Stingers and DJ");
    CHECK(strstr(s, "<Playlist Type=\"DJ\">\r\n      </Playlist>") != NULL, "an emptied list keeps its tags");
    CHECK(strstr(s, "        <Entry EventName=\"/Master/Radio/Track\" SoundName=\"EASYRADIO_pulse_2\" SampleLength=\"660035\" DisplayName=\"Long\" "
                    "Artist=\"B\" TrackStart=\"0\" DJDrop=\"5000\" TrackDrop=\"5000\" TrackBreakDown=\"330017\" StingerStart=\"658035\" "
                    "DJStart=\"659035\" End=\"660035\"/>\r\n") != NULL,
          "a long song keeps its full length and cue points");
    CHECK(strstr(s, "DisplayName=\"One & Two\"") != NULL, "raw '&' like the stock files");
    const char *ev = strstr(s, "<Playlist Type=\"EventTracks\">");
    CHECK(ev && strstr(ev, "EASYRADIO_pulse_1") && strstr(ev, "EASYRADIO_pulse_1") < strstr(ev, "EASYRADIO_pulse_2"), "EventTracks replaced too");
    out.length = 0;
    CHECK(er_edit_station(SAMPLE, strlen(SAMPLE), "Horizon Pulse", e, 1, 0, &out) == 1 && strstr(er_buf_cstr(&out), "SoundName=\"D\""),
          "dj=keep keeps DJ");
    out.length = 0;
    er_test_quiet = 1;
    CHECK(er_edit_station(SAMPLE, strlen(SAMPLE), "Ninja Tune", e, 1, 0, &out) == 0, "a missing station fails (logged)");
    CHECK(er_edit_station(SAMPLE, strlen(SAMPLE), "Bass Arena", e, 1, 0, &out) == 0, "a station without EventTracks fails");
    er_test_quiet = 0;
    /* LF-only text */
    char *lf = strdup(SAMPLE);
    size_t w = 0;
    for (size_t r = 0; lf[r]; ++r)
        if (lf[r] != '\r') lf[w++] = lf[r];
    lf[w] = 0;
    out.length = 0;
    CHECK(er_edit_station(lf, w, "Horizon Pulse", e, 2, 0, &out) == 1 && !strchr(er_buf_cstr(&out), '\r'), "LF files stay LF");
    free(lf);
    er_buf_free(&out);
}

/* ---- shuffle ---- */
static void test_shuffle(void) {
    int a[144], b[144];
    ErRandom r1 = {1}, r2 = {2}, r1b = {1};
    for (int i = 0; i < 144; ++i) a[i] = b[i] = i;
    er_shuffle(a, 144, sizeof(int), &r1);
    er_shuffle(b, 144, sizeof(int), &r2);
    int seen[144] = {0}, same = 0, moved = 0;
    for (int i = 0; i < 144; ++i) seen[a[i]]++, same += a[i] == b[i], moved += a[i] != i;
    int perm = 1;
    for (int i = 0; i < 144; ++i) perm &= seen[i] == 1;
    CHECK(perm, "a shuffle is a permutation");
    CHECK(same < 20 && moved > 120, "different seeds give different orders (same %d, moved %d)", same, moved);
    int c[144];
    for (int i = 0; i < 144; ++i) c[i] = i;
    er_shuffle(c, 144, sizeof(int), &r1b);
    CHECK(!memcmp(a, c, sizeof(a)), "the same seed gives the same order");
    /* uniform: each of 4 items lands in each position about 1/4 of the time */
    int count[4][4] = {{0}};
    ErRandom r = {12345};
    for (int t = 0; t < 40000; ++t) {
        int v[4] = {0, 1, 2, 3};
        er_shuffle(v, 4, sizeof(int), &r);
        for (int p = 0; p < 4; ++p) count[v[p]][p]++;
    }
    int uniform = 1;
    for (int x = 0; x < 4; ++x)
        for (int p = 0; p < 4; ++p) uniform &= count[x][p] > 9400 && count[x][p] < 10600;
    CHECK(uniform, "positions are uniform");
    int one = 7;
    er_shuffle(&one, 1, sizeof(int), &r);
    er_shuffle(NULL, 0, sizeof(int), &r);
    CHECK(one == 7, "one and zero items");
}

/* ---- logo ---- */
static const unsigned char PNG_8X4[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00,
    0x00, 0x04, 0x08, 0x06, 0x00, 0x00, 0x00, 0xb3, 0xcd, 0x7e, 0xf0, 0x00, 0x00, 0x00, 0x1e, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63,
    0xf8, 0xcf, 0xc0, 0xf0, 0x1f, 0x19, 0xa3, 0x71, 0xff, 0x63, 0x88, 0x60, 0x53, 0xd0, 0x80, 0x8c, 0x81, 0x82, 0xa8, 0x98, 0x90, 0x02,
    0x00, 0x56, 0xfd, 0x37, 0xd1, 0x8d, 0xc5, 0xe0, 0xac, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};

static int near(int a, int b, int tolerance) {
    return a - b <= tolerance && b - a <= tolerance;
}
static void test_logo(void) {
    uint8_t *swatch = calloc(1, ER_SWATCH_SIZE), *rgba = calloc(1, 256 * 128 * 4);
    char why[96] = "";
    /* 8x4 PNG: left red, right blue; top rows opaque, bottom rows alpha 128. Fitted: 192x96 at x=2. */
    CHECK(er_logo_swatch(PNG_8X4, sizeof(PNG_8X4), swatch, why, sizeof(why)) == 1, "PNG -> swatchbin (%s)", why);
    CHECK(er_swatch_decode(swatch, rgba) == 1, "decodes back");
    const uint8_t *red = rgba + (20 * 256 + 50) * 4, *blue_half = rgba + (80 * 256 + 150) * 4, *outside = rgba + (110 * 256 + 230) * 4;
    CHECK(near(red[0], 255, 8) && red[1] < 8 && red[2] < 8 && near(red[3], 255, 8), "opaque red stays (%d %d %d %d)", red[0], red[1], red[2], red[3]);
    CHECK(blue_half[0] < 8 && near(blue_half[2], 128, 10) && near(blue_half[3], 128, 10), "half-alpha blue is premultiplied (%d %d %d %d)",
          blue_half[0], blue_half[1], blue_half[2], blue_half[3]);
    CHECK(outside[0] == 0 && outside[3] >= 254, "outside the logo area: opaque black, as bc7.py encodes it (%d %d %d %d)", outside[0], outside[1], outside[2], outside[3]);
    const uint8_t *margin = rgba + (50 * 256 + 0) * 4;
    CHECK(margin[3] < 8, "the side margin of a 2:1 image is transparent (%d)", margin[3]);
    /* BMP: 32-bit with alpha (BITMAPV4-less, plain 32-bit: stb reads alpha when present) */
    Bytes bmp = {0};
    {
        int w = 300, h = 300;
        unsigned char head[54] = {'B', 'M'};
        uint32_t size = 54 + (uint32_t)(w * h * 4);
        memcpy(head + 2, &size, 4);
        uint32_t off = 54, dib = 40, planes_bits = 1 | (32 << 16), image = (uint32_t)(w * h * 4);
        memcpy(head + 10, &off, 4);
        memcpy(head + 14, &dib, 4);
        memcpy(head + 18, &w, 4);
        memcpy(head + 22, &h, 4);
        memcpy(head + 26, &planes_bits, 4);
        memcpy(head + 34, &image, 4);
        add(&bmp, head, 54);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                unsigned char px[4] = {(unsigned char)(x < 150 ? 255 : 0), (unsigned char)(y < 150 ? 255 : 0), 0, 255};
                add(&bmp, px, 4);
            }
    }
    CHECK(er_logo_swatch(bmp.d, bmp.n, swatch, why, sizeof(why)) == 1, "a 300x300 BMP fits 96x96 (%s)", why);
    free(bmp.d);
    unsigned char garbage[100] = "not an image";
    CHECK(er_logo_swatch(garbage, sizeof(garbage), swatch, why, sizeof(why)) == 0, "garbage is refused: %s", why);
    free(swatch);
    free(rgba);
}

/* ---- generator on a synthetic mod ---- */
static void write_file(const char *path, const void *data, size_t n) {
    FILE *f = fopen(path, "wb");
    fwrite(data, 1, n, f);
    fclose(f);
}
static int exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}
static void test_generate(void) {
    char root[] = "/tmp/easyradio-test-XXXXXX";
    if (!mkdtemp(root)) return;
    char p[1024], mod[600], out[600], cache[600], game[600];
    snprintf(mod, sizeof(mod), "%s/mod/", root);
    snprintf(out, sizeof(out), "%s/mod/plugins/generated/", root);
    snprintf(cache, sizeof(cache), "%s/mod/plugins/cache/", root);
    snprintf(game, sizeof(game), "%s/game", root);
    snprintf(p, sizeof(p), "%s/game/Mount/media/audio/", root);
    er_make_dirs(p);
    for (int i = 0; i < ER_LANGUAGES; ++i) {
        if (i == 3) continue; /* ES missing: that language is skipped */
        snprintf(p, sizeof(p), "%s/game/Mount/media/audio/RadioInfo_%s.xml", root, er_languages[i]);
        write_file(p, SAMPLE, strlen(SAMPLE));
    }
    snprintf(p, sizeof(p), "%s/mod/party/", root);
    er_make_dirs(p);
    snprintf(p, sizeof(p), "%s/mod/plugins/generated/ui/textures/Anthem/HUD/", root);
    er_make_dirs(p);
    snprintf(p, sizeof(p), "%s/mod/plugins/generated/ui/textures/Anthem/HUD/HUD_RadioIcon_Bass.swatchbin", root);
    write_file(p, "stale", 5); /* a station no longer mapped loses its old icon */
    const char *names[] = {"b - Second.mp3", "A - First.MP3", "c - broken.mp3", "d - short.mp3", "notes.txt"};
    for (int i = 0; i < 5; ++i) {
        Bytes b = {0};
        if (i < 2) {
            id3(&b, 3, 3, i ? "Artist A - First (Official Video)" : "Second", i ? NULL : "B Artist", 0);
            xing_frame(&b, i ? 26000 : 9000, "Xing");
            frames(&b, 5);
        } else if (i == 2) {
            add_zero(&b, 9000);
        } else if (i == 3) {
            frames(&b, 100);
        } else {
            add(&b, "hello", 5);
        }
        snprintf(p, sizeof(p), "%s/mod/party/%s", root, names[i]);
        write_file(p, b.d, b.n);
        free(b.d);
    }
    snprintf(p, sizeof(p), "%s/mod/party/logo.png", root);
    write_file(p, PNG_8X4, sizeof(PNG_8X4));
    snprintf(p, sizeof(p), "%s/mod/easyradio.ini", root);
    const char *ini = "[radios]\nhorizon=party\nxs=missing-folder\nninjatune=empty\n";
    write_file(p, ini, strlen(ini));
    snprintf(p, sizeof(p), "%s/mod/empty/", root);
    er_make_dirs(p);

    const char *dirs[] = {game, NULL};
    ErOptions o = {mod, out, cache, dirs, '/', 0, 1, NULL};
    ErResult r;
    er_test_quiet = 1;
    int ok = er_generate(&o, &r);
    CHECK(ok == 1 && r.count == 2 && r.station_songs[0] == 2 && !r.station_songs[2] && !r.station_songs[5], "2 good songs on pulse; bad files, a "
                                                                                                             "missing and an empty folder skipped");
    CHECK(r.languages == 9 && r.logos == 1, "9 languages (ES missing), 1 logo (%u, %u)", r.languages, r.logos);
    if (r.count == 2) {
        CHECK(!strcmp(r.songs[0].key, "EASYRADIO_pulse_1") && !strcmp(r.songs[0].title, "First") && !strcmp(r.songs[0].artist, "Artist A") &&
                  r.songs[0].ms == ms_of(26000) && r.songs[0].number == 1,
              "file order without shuffle: '%s' '%s' %llu", r.songs[0].title, r.songs[0].artist, (unsigned long long)r.songs[0].ms);
        CHECK(!strcmp(r.songs[1].artist, "B Artist") && !strcmp(r.songs[1].title, "Second"), "tags without ' - ': %s / %s", r.songs[1].artist,
              r.songs[1].title);
    }
    snprintf(p, sizeof(p), "%saudio/RadioInfo_EN.xml", out);
    CHECK(exists(p), "RadioInfo_EN.xml written");
    snprintf(p, sizeof(p), "%saudio/RadioInfo_ES.xml", out);
    CHECK(!exists(p), "no RadioInfo_ES.xml");
    snprintf(p, sizeof(p), "%sui/textures/Anthem/HUD/HUD_RadioIcon_Pulse.swatchbin", out);
    CHECK(exists(p), "Pulse icon written");
    snprintf(p, sizeof(p), "%sui/textures/Anthem/HUD/HUD_RadioIcon_Bass.swatchbin", out);
    CHECK(!exists(p), "the stale Bass icon is gone");
    er_result_free(&r);

    /* shuffle with the cache: same songs */
    ErOptions o2 = {mod, out, cache, dirs, '/', 1, 99, NULL};
    ok = er_generate(&o2, &r);
    CHECK(ok == 1 && r.count == 2, "second run (cache, shuffled) ok");
    er_result_free(&r);

    /* the music folder (<Azimuth>/EasyRadio/) first, then the mod's; an absolute folder; a missing name is created there */
    char music[600];
    snprintf(music, sizeof(music), "%s/EasyRadio/", root);
    snprintf(p, sizeof(p), "%s/EasyRadio/party/", root);
    er_make_dirs(p);
    snprintf(p, sizeof(p), "%s/EasyRadio/party/a - Only.mp3", root);
    {
        Bytes b = {0};
        xing_frame(&b, 13000, "Xing");
        frames(&b, 5);
        write_file(p, b.d, b.n);
        free(b.d);
    }
    snprintf(p, sizeof(p), "%s/mod/easyradio.ini", root);
    char ini3[1400];
    snprintf(ini3, sizeof(ini3), "[radios]\nhorizon=party\nbass=%s/mod/party/\nxs=music2\nlevante=stock\n", root);
    write_file(p, ini3, strlen(ini3));
    ErOptions o3 = {mod, out, cache, dirs, '/', 0, 1, music};
    ok = er_generate(&o3, &r);
    CHECK(ok == 1 && r.station_songs[0] == 1 && r.station_songs[1] == 2 && !r.station_songs[2] && !r.station_songs[3],
          "pulse from the music folder (1), bass from an absolute folder (2): %u %u", r.station_songs[0], r.station_songs[1]);
    snprintf(p, sizeof(p), "%s/EasyRadio/music2", root);
    CHECK(exists(p), "a missing radio folder is created, empty, in the music folder");
    er_result_free(&r);

    /* nothing valid: stock, nothing written */
    snprintf(p, sizeof(p), "%s/mod/easyradio.ini", root);
    const char *ini2 = "[radios]\nbass=empty\n";
    write_file(p, ini2, strlen(ini2));
    ok = er_generate(&o, &r);
    snprintf(p, sizeof(p), "%saudio/RadioInfo_EN.xml", out);
    CHECK(ok == 0 && !exists(p), "nothing playable: stock (and the old files removed)");
    er_result_free(&r);
    /* no ini */
    snprintf(p, sizeof(p), "%s/mod/easyradio.ini", root);
    unlink(p);
    ok = er_generate(&o, &r);
    CHECK(ok == 0, "no easyradio.ini: stock");
    er_result_free(&r);
    er_test_quiet = 0;
    snprintf(p, sizeof(p), "rm -rf '%s'", root);
    if (system(p)) printf("note: could not remove %s\n", root);
}

int main(void) {
    test_duration();
    test_tags();
    test_names();
    test_ini();
    test_xml();
    test_shuffle();
    test_logo();
    test_generate();
    printf("%s: %d checks, %d failed\n", failed ? "FAIL" : "PASS", checks, failed);
    return failed ? 1 : 0;
}
