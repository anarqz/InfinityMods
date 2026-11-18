/* EasyRadio core: ID3 tags and MP3 duration. A port of the Python prototype (parse_header,
   skip_id3v2, mp3_duration_ms, id3v2_text, id3v1_text), whose durations matched FMOD's to the millisecond live
   (run 4: "FMOD length 299016 ms ... XML End 299016 ms"). */
#include "er_core.h"

#include <string.h>

/* ---- MPEG audio frame headers ---- */
static const uint16_t bitrates[6][15] = {
    {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320},     /* MPEG1 Layer III */
    {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},    /* MPEG1 Layer II */
    {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448}, /* MPEG1 Layer I */
    {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},         /* MPEG2/2.5 Layer III */
    {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},         /* MPEG2/2.5 Layer II */
    {0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256},    /* MPEG2/2.5 Layer I */
};
static const uint32_t rates[4][3] = {{11025, 12000, 8000}, {0, 0, 0}, {22050, 24000, 16000}, {44100, 48000, 32000}};

typedef struct {
    uint32_t size, samples, rate;
    int version, layer, mode;
} Header;

/* parse_header: data[i..i+4) within a buffer of size bytes */
static int parse_header(const uint8_t *b, size_t size, size_t i, Header *h) {
    if (i + 4 > size || b[i] != 0xff || (b[i + 1] & 0xe0) != 0xe0) return 0;
    int version = (b[i + 1] >> 3) & 3, layer = (b[i + 1] >> 1) & 3;
    int bitrate_index = b[i + 2] >> 4, rate_index = (b[i + 2] >> 2) & 3, padding = (b[i + 2] >> 1) & 1;
    if (version == 1 || layer == 0 || bitrate_index == 0 || bitrate_index == 15 || rate_index == 3) return 0;
    uint32_t kbps = bitrates[(version == 3 ? 0 : 3) + (layer == 1 ? 0 : layer == 2 ? 1 : 2)][bitrate_index];
    uint32_t rate = rates[version][rate_index];
    if (layer == 3) {
        h->samples = 384;
        h->size = (12 * kbps * 1000 / rate + (uint32_t)padding) * 4;
    } else if (layer == 2 || version == 3) {
        h->samples = 1152;
        h->size = 144 * kbps * 1000 / rate + (uint32_t)padding;
    } else {
        h->samples = 576;
        h->size = 72 * kbps * 1000 / rate + (uint32_t)padding;
    }
    h->rate = rate;
    h->version = version;
    h->layer = layer;
    h->mode = b[i + 3] >> 6;
    return 1;
}

static uint32_t syncsafe(const uint8_t *p) {
    return (uint32_t)p[0] << 21 | (uint32_t)p[1] << 14 | (uint32_t)p[2] << 7 | p[3];
}
static uint32_t big32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

size_t er_id3v2_skip(const uint8_t *b, size_t size) {
    size_t at = 0;
    while (at + 10 <= size && b[at] == 'I' && b[at + 1] == 'D' && b[at + 2] == '3') {
        at += 10 + (size_t)syncsafe(b + at + 6) + ((b[at + 5] & 0x10) ? 10 : 0);
    }
    return at;
}

/* The first frame followed by a second valid frame, from start; Xing/Info/VBRI; else the frame scan.
   full = 0: data is only a prefix (then -1 whenever the answer could depend on what follows). */
static int duration(const uint8_t *data, size_t length, uint64_t total, int full, uint64_t *ms) {
    size_t start = er_id3v2_skip(data, length);
    /* a prefix must show where the tags end (a further "ID3" would be skipped too) */
    if (!full && start + 10 > length) return -1;
    size_t end = (size_t)total;
    if (full && total >= 128 && data[total - 128] == 'T' && data[total - 127] == 'A' && data[total - 126] == 'G') end -= 128;
    size_t i = start;
    Header h, next;
    int found = 0;
    while (i + 4 < end) { /* i < end - 4 */
        if (!full && i + 4 > length) return -1;
        if (parse_header(data, length, i, &h)) {
            if (!full && i + h.size + 4 > length) return -1;
            if (parse_header(data, length, i + h.size, &next)) {
                found = 1;
                break;
            }
        }
        ++i;
    }
    if (!found) return full ? 0 : -1;
    if (h.layer == 1) {
        size_t side = h.version == 3 ? (h.mode != 3 ? 32 : 17) : (h.mode != 3 ? 17 : 9);
        size_t x = i + 4 + side;
        if (!full && x + 12 > length) return -1;
        if (x + 4 <= length && (!memcmp(data + x, "Xing", 4) || !memcmp(data + x, "Info", 4))) {
            uint32_t flags = x + 8 <= length ? big32(data + x + 4) : 0;
            if (flags & 1) {
                uint32_t frames = x + 12 <= length ? big32(data + x + 8) : 0;
                *ms = (uint64_t)frames * h.samples * 1000 / h.rate;
                return 1;
            }
        }
        size_t v = i + 4 + 32;
        if (!full && v + 18 > length) return -1;
        if (v + 4 <= length && !memcmp(data + v, "VBRI", 4)) {
            uint32_t frames = v + 18 <= length ? big32(data + v + 14) : 0;
            *ms = (uint64_t)frames * h.samples * 1000 / h.rate;
            return 1;
        }
    }
    if (!full) return -1;
    uint32_t rate = h.rate;
    uint64_t samples = 0;
    while (i + 4 < end) {
        Header f;
        if (!parse_header(data, length, i, &f) || f.rate != rate) {
            ++i; /* resync (junk between frames) */
            continue;
        }
        samples += f.samples;
        i += f.size;
    }
    *ms = samples * 1000 / rate;
    return 1;
}

int er_mp3_duration_ms(const uint8_t *data, size_t size, uint64_t *ms) {
    return duration(data, size, size, 1, ms);
}

int er_mp3_duration_prefix(const uint8_t *data, size_t length, uint64_t total_size, uint64_t *ms) {
    if (length >= total_size) return duration(data, (size_t)total_size, total_size, 1, ms);
    /* Only the Xing/VBRI answer is final from a prefix, and only when the first frame pair lies before the ID3v1
       tag (a 128-byte tag at the end can only shorten the search bound, which matters for files under 1 frame). */
    if (total_size < length + 128 + 8) return -1;
    return duration(data, length, total_size, 0, ms);
}

/* ---- tags ---- */
void er_tags_free(ErTags *tags) {
    er_text_free(&tags->title);
    er_text_free(&tags->artist);
}

/* value.split("\0")[0].strip() */
static void first_value(ErText *t) {
    for (size_t i = 0; i < t->n; ++i)
        if (!t->cp[i]) {
            t->n = i;
            break;
        }
    size_t a = 0, b = t->n;
    while (a < b && er_is_space(t->cp[a])) ++a;
    while (b > a && er_is_space(t->cp[b - 1])) --b;
    if (a) memmove(t->cp, t->cp + a, (b - a) * sizeof(uint32_t));
    t->n = b - a;
}

static void set_text(ErText *into, const ErText *value) {
    into->n = 0;
    for (size_t i = 0; i < value->n; ++i) er_text_push(into, value->cp[i]);
}

void er_id3v2_text(const uint8_t *data, size_t size, ErTags *tags) {
    if (size < 10 || data[0] != 'I' || data[1] != 'D' || data[2] != '3') return;
    int major = data[3], flags = data[5];
    size_t body_size = syncsafe(data + 6);
    const uint8_t *body = data + 10;
    if (body_size > size - 10) body_size = size - 10;
    if (flags & 0x80) return; /* unsynchronisation: not handled (as the prototype) */
    size_t at = 0;
    if ((flags & 0x40) && major >= 3) {
        if (body_size < 4) return;
        uint32_t ext = big32(body);
        at = major == 4 ? ext : (size_t)ext + 4;
    }
    size_t head = major == 2 ? 6 : 10;
    ErText value = {0};
    while (at + head <= body_size) {
        const uint8_t *f = body + at;
        size_t id_length = major == 2 ? 3 : 4, fsize;
        int empty = 1;
        for (size_t k = 0; k < id_length; ++k)
            if (f[k]) empty = 0;
        if (major == 2) fsize = (size_t)f[3] << 16 | (size_t)f[4] << 8 | f[5];
        else fsize = major == 4 ? syncsafe(f + 4) : big32(f + 4);
        if (empty || !fsize) break;
        size_t content_at = at + head, content_size = fsize;
        if (content_at > body_size) content_size = 0;
        else if (content_size > body_size - content_at) content_size = body_size - content_at;
        int is_title = major == 2 ? !memcmp(f, "TT2", 3) : !memcmp(f, "TIT2", 4);
        int is_artist = major == 2 ? !memcmp(f, "TP1", 3) : !memcmp(f, "TPE1", 4);
        if ((is_title || is_artist) && content_size) {
            const uint8_t *c = body + content_at;
            int encoding = c[0], ok;
            value.n = 0;
            switch (encoding) {
            case 0: ok = er_decode_latin1(&value, c + 1, content_size - 1); break;
            case 1: ok = er_decode_utf16(&value, c + 1, content_size - 1, 0, 1); break;
            case 2: ok = er_decode_utf16(&value, c + 1, content_size - 1, 1, 0); break;
            case 3: ok = er_decode_utf8(&value, c + 1, content_size - 1); break;
            default: ok = 0; break;
            }
            if (!ok) value.n = 0;
            first_value(&value);
            if (value.n) set_text(is_title ? &tags->title : &tags->artist, &value);
        }
        at += head + fsize;
    }
    er_text_free(&value);
}

void er_id3v1_text(const uint8_t *data, size_t size, ErTags *tags) {
    if (size < 128) return;
    const uint8_t *tag = data + size - 128;
    if (tag[0] != 'T' || tag[1] != 'A' || tag[2] != 'G') return;
    for (int which = 0; which < 2; ++which) {
        const uint8_t *field = tag + (which ? 33 : 3);
        size_t n = 0;
        while (n < 30 && field[n]) ++n;
        ErText value = {0};
        er_decode_latin1(&value, field, n);
        first_value(&value);
        set_text(which ? &tags->artist : &tags->title, &value);
        er_text_free(&value);
    }
}
