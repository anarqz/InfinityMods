/* EasyRadio core: the RadioInfo text edit. A port of the Python prototype's edit_station, which ran live:
   only the mapped station's FreeroamTracks and EventTracks change (and with dj=off its Stingers, ShortStingers
   and DJ lists empty); every other byte stays. '&' stays raw, as in the stock files (the game reads them so). */
#include "er_core.h"

#include <string.h>

static int starts(const char *p, const char *end, const char *literal) {
    size_t n = er_strlen(literal);
    return (size_t)(end - p) >= n && !memcmp(p, literal, n);
}
static int word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}
static const char *find(const char *p, const char *end, const char *literal) {
    for (; p < end; ++p)
        if (starts(p, end, literal)) return p;
    return NULL;
}

static uint64_t cue(uint64_t end, int64_t value) {
    if (value < 0) return 0;
    return (uint64_t)value > end ? end : (uint64_t)value;
}

static void entry_line(ErBuf *out, const char *indent, size_t indent_length, const ErEntry *e) {
    uint64_t end = e->ms;
    er_buf_bytes(out, indent, indent_length);
    er_buf_str(out, "<Entry EventName=\"/Master/Radio/Track\" SoundName=\"");
    er_buf_str(out, e->key);
    er_buf_format(out, "\" SampleLength=\"%llu\" DisplayName=\"", (unsigned long long)end);
    er_buf_str(out, e->title);
    er_buf_str(out, "\" Artist=\"");
    er_buf_str(out, e->artist);
    er_buf_format(out, "\" TrackStart=\"0\" DJDrop=\"%llu\" TrackDrop=\"%llu\" TrackBreakDown=\"%llu\" StingerStart=\"%llu\" DJStart=\"%llu\" End=\"%llu\"/>",
                  (unsigned long long)cue(end, 5000), (unsigned long long)cue(end, 5000), (unsigned long long)cue(end, (int64_t)(end / 2)),
                  (unsigned long long)cue(end, (int64_t)end - 2000), (unsigned long long)cue(end, (int64_t)end - 1000), (unsigned long long)end);
}

/* <Playlist Type="(\w+)">\r?\n([ \t]*)<(?!/).*?\r?\n([ \t]*)</Playlist> at p; 1 with the parts */
typedef struct {
    const char *kind, *indent, *closing_indent, *end;
    size_t kind_length, indent_length, closing_length;
} Playlist;
static int playlist_at(const char *p, const char *end, Playlist *m) {
    static const char open[] = "<Playlist Type=\"";
    if (!starts(p, end, open)) return 0;
    const char *k = p + sizeof(open) - 1, *q = k;
    while (q < end && word_char(*q)) ++q;
    if (q == k || !starts(q, end, "\">")) return 0;
    m->kind = k;
    m->kind_length = (size_t)(q - k);
    q += 2;
    if (q < end && *q == '\r') ++q;
    if (q >= end || *q != '\n') return 0;
    ++q;
    m->indent = q;
    while (q < end && (*q == ' ' || *q == '\t')) ++q;
    m->indent_length = (size_t)(q - m->indent);
    if (q >= end || *q != '<' || (q + 1 < end && q[1] == '/')) return 0;
    for (const char *r = q + 1; r < end; ++r) {
        const char *s = r;
        if (*s == '\r') ++s;
        if (s >= end || *s != '\n') continue;
        ++s;
        const char *ws = s;
        while (s < end && (*s == ' ' || *s == '\t')) ++s;
        if (starts(s, end, "</Playlist>")) {
            m->closing_indent = ws;
            m->closing_length = (size_t)(s - ws);
            m->end = s + 11;
            return 1;
        }
    }
    return 0;
}

static int kind_is(const Playlist *m, const char *name) {
    return m->kind_length == er_strlen(name) && !memcmp(m->kind, name, m->kind_length);
}

int er_edit_station(const char *text, size_t length, const char *station, const ErEntry *entries, size_t count, int dj_off, ErBuf *out) {
    const char *end = text + length;
    const char *newline = find(text, end, "\r\n") ? "\r\n" : "\n";
    ErBuf head = {0};
    er_buf_format(&head, "<RadioStation Name=\"%s\"", station);
    const char *body = NULL, *body_end = NULL;
    for (const char *p = text; (p = find(p, end, er_buf_cstr(&head))) != NULL; ++p) {
        const char *q = p + head.length;
        while (q < end && *q != '>') ++q;
        if (q >= end) break;
        const char *close = find(q + 1, end, "</RadioStation>");
        if (close) {
            body = q + 1;
            body_end = close;
            break;
        }
    }
    er_buf_free(&head);
    if (!body) {
        er_log("WARN: RadioInfo: station '%s' not found", station);
        return 0;
    }
    size_t start_length = out->length;
    er_buf_bytes(out, text, (size_t)(body - text));
    int freeroam = 0, event = 0;
    for (const char *p = body; p < body_end;) {
        Playlist m;
        if (!playlist_at(p, body_end, &m)) {
            er_buf_char(out, *p++);
            continue;
        }
        int tracks = kind_is(&m, "FreeroamTracks") || kind_is(&m, "EventTracks");
        int emptied = dj_off && (kind_is(&m, "Stingers") || kind_is(&m, "ShortStingers") || kind_is(&m, "DJ"));
        if (!tracks && !emptied) {
            er_buf_bytes(out, p, (size_t)(m.end - p));
            p = m.end;
            continue;
        }
        freeroam |= kind_is(&m, "FreeroamTracks");
        event |= kind_is(&m, "EventTracks");
        er_buf_str(out, "<Playlist Type=\"");
        er_buf_bytes(out, m.kind, m.kind_length);
        er_buf_str(out, "\">");
        er_buf_str(out, newline);
        if (tracks) {
            for (size_t i = 0; i < count; ++i) {
                if (i) er_buf_str(out, newline);
                entry_line(out, m.indent, m.indent_length, &entries[i]);
            }
            if (count) er_buf_str(out, newline);
        }
        er_buf_bytes(out, m.closing_indent, m.closing_length);
        er_buf_str(out, "</Playlist>");
        p = m.end;
    }
    er_buf_bytes(out, body_end, (size_t)(end - body_end));
    if (!freeroam || !event) {
        er_log("WARN: RadioInfo: station '%s' has no %s list", station, freeroam ? "EventTracks" : "FreeroamTracks");
        out->length = start_length;
        return 0;
    }
    return !out->failed;
}
