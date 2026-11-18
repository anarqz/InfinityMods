/* EasyRadio core: HUD text. A port of the Python prototype's cleaner (ascii_text,
   strip_noise, uploader_artist, split_artist_title, hud_names), which ran live. Text is held as code points so Python's Unicode rules (\s, \w, str.lower) carry over exactly; the
   tables come from gen_unicode.py. */
#include "er_core.h"
#include "er_unicode_tables.h"

#include <string.h>

/* ---- ErText ---- */
void er_text_free(ErText *t) {
    er_free(t->cp);
    t->cp = NULL;
    t->n = t->capacity = 0;
}
void er_text_push(ErText *t, uint32_t cp) {
    if (t->n == t->capacity) {
        size_t capacity = t->capacity ? t->capacity * 2 : 64;
        uint32_t *grown = er_realloc(t->cp, t->capacity * sizeof(uint32_t), capacity * sizeof(uint32_t));
        if (!grown) return;
        t->cp = grown;
        t->capacity = capacity;
    }
    t->cp[t->n++] = cp;
}
static void text_slice(ErText *out, const ErText *in, size_t from, size_t to) {
    out->n = 0;
    for (size_t i = from; i < to && i < in->n; ++i) er_text_push(out, in->cp[i]);
}
static void text_append(ErText *out, const ErText *in, size_t from, size_t to) {
    for (size_t i = from; i < to && i < in->n; ++i) er_text_push(out, in->cp[i]);
}
static void text_append_ascii(ErText *out, const char *s) {
    while (*s) er_text_push(out, (unsigned char)*s++);
}
static void text_copy(ErText *out, const ErText *in) {
    text_slice(out, in, 0, in->n);
}

/* ---- decoders (Python codecs, strict) ---- */
int er_decode_latin1(ErText *t, const uint8_t *s, size_t n) {
    for (size_t i = 0; i < n; ++i) er_text_push(t, s[i]);
    return 1;
}
static int utf8_next(const uint8_t *s, size_t n, size_t *i, uint32_t *cp) {
    uint8_t b = s[*i];
    if (b < 0x80) {
        *cp = b;
        *i += 1;
        return 1;
    }
    int extra;
    uint32_t value, min;
    if (b >= 0xc2 && b <= 0xdf) extra = 1, value = b & 0x1f, min = 0x80;
    else if (b >= 0xe0 && b <= 0xef) extra = 2, value = b & 0x0f, min = 0x800;
    else if (b >= 0xf0 && b <= 0xf4) extra = 3, value = b & 0x07, min = 0x10000;
    else return 0;
    for (int k = 1; k <= extra; ++k) {
        if (*i + (size_t)k >= n) return 0;
        uint8_t c = s[*i + (size_t)k];
        if ((c & 0xc0) != 0x80) return 0;
        value = (value << 6) | (c & 0x3f);
    }
    if (value < min || value > 0x10ffff || (value >= 0xd800 && value < 0xe000)) return 0;
    *cp = value;
    *i += (size_t)extra + 1;
    return 1;
}
int er_decode_utf8(ErText *t, const uint8_t *s, size_t n) {
    size_t start = t->n;
    for (size_t i = 0; i < n;) {
        uint32_t cp;
        if (!utf8_next(s, n, &i, &cp)) {
            t->n = start;
            return 0;
        }
        er_text_push(t, cp);
    }
    return 1;
}
void er_text_from_utf8_lossy(ErText *t, const char *s, size_t n) {
    const uint8_t *u = (const uint8_t *)s;
    for (size_t i = 0; i < n;) {
        uint32_t cp;
        if (!utf8_next(u, n, &i, &cp)) {
            cp = 0xfffd;
            ++i;
        }
        er_text_push(t, cp);
    }
}
int er_decode_utf16(ErText *t, const uint8_t *s, size_t n, int big_endian, int bom_detect) {
    size_t start = t->n, i = 0;
    if (bom_detect && n >= 2) {
        if (s[0] == 0xff && s[1] == 0xfe) big_endian = 0, i = 2;
        else if (s[0] == 0xfe && s[1] == 0xff) big_endian = 1, i = 2;
    }
    if ((n - i) % 2) goto fail; /* "truncated data" */
    for (; i < n; i += 2) {
        uint32_t u = big_endian ? (uint32_t)(s[i] << 8 | s[i + 1]) : (uint32_t)(s[i + 1] << 8 | s[i]);
        if (u >= 0xd800 && u < 0xdc00) {
            if (i + 4 > n) goto fail;
            uint32_t v = big_endian ? (uint32_t)(s[i + 2] << 8 | s[i + 3]) : (uint32_t)(s[i + 3] << 8 | s[i + 2]);
            if (v < 0xdc00 || v >= 0xe000) goto fail;
            u = 0x10000 + ((u - 0xd800) << 10) + (v - 0xdc00);
            i += 2;
        } else if (u >= 0xdc00 && u < 0xe000) {
            goto fail;
        }
        er_text_push(t, u);
    }
    return 1;
fail:
    t->n = start;
    return 0;
}
void er_text_to_utf8(const ErText *t, ErBuf *out) {
    for (size_t i = 0; i < t->n; ++i) {
        uint32_t c = t->cp[i];
        char b[4];
        size_t k;
        if (c < 0x80) b[0] = (char)c, k = 1;
        else if (c < 0x800) b[0] = (char)(0xc0 | c >> 6), b[1] = (char)(0x80 | (c & 0x3f)), k = 2;
        else if (c < 0x10000) b[0] = (char)(0xe0 | c >> 12), b[1] = (char)(0x80 | ((c >> 6) & 0x3f)), b[2] = (char)(0x80 | (c & 0x3f)), k = 3;
        else
            b[0] = (char)(0xf0 | c >> 18), b[1] = (char)(0x80 | ((c >> 12) & 0x3f)), b[2] = (char)(0x80 | ((c >> 6) & 0x3f)),
            b[3] = (char)(0x80 | (c & 0x3f)), k = 4;
        er_buf_bytes(out, b, k);
    }
}

/* ---- character classes ---- */
int er_is_space(uint32_t cp) {
    for (size_t i = 0; i < sizeof(er_space_points) / sizeof(er_space_points[0]); ++i)
        if (er_space_points[i] == cp) return 1;
    return 0;
}
int er_is_word(uint32_t cp) {
    size_t lo = 0, hi = sizeof(er_word_ranges) / sizeof(er_word_ranges[0]);
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (cp < er_word_ranges[mid].first) hi = mid;
        else if (cp > er_word_ranges[mid].last) lo = mid + 1;
        else return 1;
    }
    return 0;
}
static const ErMapRow *map_find(const ErMapRow *rows, size_t count, uint32_t cp) {
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (cp < rows[mid].cp) hi = mid;
        else if (cp > rows[mid].cp) lo = mid + 1;
        else return &rows[mid];
    }
    return NULL;
}
void er_text_lower(const ErText *in, ErText *out) {
    out->n = 0;
    for (size_t i = 0; i < in->n; ++i) {
        const ErMapRow *row = map_find(er_lower_rows, sizeof(er_lower_rows) / sizeof(er_lower_rows[0]), in->cp[i]);
        if (!row) {
            er_text_push(out, in->cp[i]);
            continue;
        }
        for (unsigned k = 0; k < row->length; ++k) er_text_push(out, er_lower_pool[row->offset + k]);
    }
}
static uint32_t lower_ascii(uint32_t c) {
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* ---- ascii_text ---- */
void er_ascii_text(const ErText *in, ErBuf *out) {
    ErBuf raw = {0};
    for (size_t i = 0; i < in->n; ++i) {
        uint32_t c = in->cp[i];
        if (c < 0x80) {
            if (c == '"') er_buf_char(&raw, '\'');
            else if (c >= ' ' && c <= '~' && c != '<' && c != '>') er_buf_char(&raw, (char)c);
            continue;
        }
        const ErMapRow *row = map_find(er_fold_rows, sizeof(er_fold_rows) / sizeof(er_fold_rows[0]), c);
        if (row) er_buf_bytes(&raw, er_fold_pool + row->offset, row->length);
    }
    /* re.sub(r"\s+", " ").strip(): only ' ' is left as whitespace */
    size_t start = out->length;
    int pending_space = 0;
    for (size_t i = 0; i < raw.length; ++i) {
        if (raw.data[i] == ' ') {
            pending_space = 1;
            continue;
        }
        if (pending_space && out->length > start) er_buf_char(out, ' ');
        pending_space = 0;
        er_buf_char(out, raw.data[i]);
    }
    er_buf_cstr(out);
    er_buf_free(&raw);
}
static void ascii_of(const ErText *in, ErBuf *out) {
    out->length = 0;
    er_ascii_text(in, out);
    er_buf_cstr(out);
}

/* ---- small matchers (re.I on ASCII literals) ---- */
static int match_ci(const ErText *t, size_t at, const char *literal) {
    size_t i = 0;
    for (; literal[i]; ++i)
        if (at + i >= t->n || lower_ascii(t->cp[at + i]) != (uint32_t)literal[i]) return 0;
    return 1;
}
static int word_at(const ErText *t, size_t i) {
    return i < t->n && er_is_word(t->cp[i]);
}
/* \b at position i */
static int boundary(const ErText *t, size_t i) {
    int before = i > 0 && er_is_word(t->cp[i - 1]);
    return before != word_at(t, i);
}
static void text_strip(ErText *t) {
    size_t a = 0, b = t->n;
    while (a < b && er_is_space(t->cp[a])) ++a;
    while (b > a && er_is_space(t->cp[b - 1])) --b;
    if (a) memmove(t->cp, t->cp + a, (b - a) * sizeof(uint32_t));
    t->n = b - a;
}
static int text_equal(const ErText *a, const ErText *b) {
    return a->n == b->n && (!a->n || !memcmp(a->cp, b->cp, a->n * sizeof(uint32_t)));
}
static int text_contains(const ErText *hay, const ErText *needle) {
    if (!needle->n) return 1;
    for (size_t i = 0; i + needle->n <= hay->n; ++i)
        if (!memcmp(hay->cp + i, needle->cp, needle->n * sizeof(uint32_t))) return 1;
    return 0;
}
/* index of literal (exact) at or after from, or t->n */
static size_t find_literal(const ErText *t, size_t from, const char *literal) {
    size_t length = er_strlen(literal);
    for (size_t i = from; i + length <= t->n; ++i) {
        size_t k = 0;
        while (k < length && t->cp[i + k] == (uint32_t)(unsigned char)literal[k]) ++k;
        if (k == length) return i;
    }
    return t->n;
}

/* NOISE = official|video|audio|\bhd\b|\b4k\b|remaster|oficial|ultra music|lyrics?\b|visuali[sz]er (re.I), searched */
static int noise_search(const ErText *t) {
    static const char *const plain[] = {"official", "video", "audio", "remaster", "oficial", "ultra music"};
    for (size_t i = 0; i <= t->n; ++i) {
        for (size_t k = 0; k < sizeof(plain) / sizeof(plain[0]); ++k)
            if (match_ci(t, i, plain[k])) return 1;
        if (boundary(t, i) && (match_ci(t, i, "hd") || match_ci(t, i, "4k")) && boundary(t, i + 2)) return 1;
        if (match_ci(t, i, "lyric") && ((i + 5 < t->n && lower_ascii(t->cp[i + 5]) == 's' && boundary(t, i + 6)) || boundary(t, i + 5)))
            return 1;
        if (match_ci(t, i, "visuali") && i + 7 < t->n && (lower_ascii(t->cp[i + 7]) == 's' || lower_ascii(t->cp[i + 7]) == 'z') &&
            match_ci(t, i + 8, "er"))
            return 1;
    }
    return 0;
}

/* (?:feat\.?|ft\.?|featuring)\s+ at i (re.I): end of the whitespace run, or 0 */
static size_t feat_at(const ErText *t, size_t i) {
    static const char *const words[] = {"feat", "ft", "featuring"};
    for (size_t w = 0; w < 3; ++w) {
        if (!match_ci(t, i, words[w])) continue;
        size_t j = i + er_strlen(words[w]);
        int dot_allowed = w < 2;
        for (int with_dot = dot_allowed; with_dot >= 0; --with_dot) {
            size_t k = j;
            if (with_dot) {
                if (k >= t->n || t->cp[k] != '.') continue;
                ++k;
            }
            if (k < t->n && er_is_space(t->cp[k])) {
                while (k < t->n && er_is_space(t->cp[k])) ++k;
                return k;
            }
        }
    }
    return 0;
}
/* FEAT = \s*\b(?:feat\.?|ft\.?|featuring)\s+ searched from `from`: 1 with [*start, *end) */
static int feat_search(const ErText *t, size_t from, size_t *start, size_t *end) {
    for (size_t p = from; p < t->n; ++p) {
        size_t j = p;
        while (j < t->n && er_is_space(t->cp[j])) ++j;
        if (!boundary(t, j)) continue;
        size_t e = feat_at(t, j);
        if (e) {
            *start = p;
            *end = e;
            return 1;
        }
    }
    return 0;
}

static int is_open(uint32_t c) {
    return c == '(' || c == '[';
}
static int is_close(uint32_t c) {
    return c == ')' || c == ']';
}

void er_strip_noise(const ErText *in, ErText *out) {
    ErText a = {0}, b = {0}, c = {0};
    /* 1. \s*[\(\[]([^\(\)\[\]]*)[\)\]] -> "" when NOISE finds the group */
    for (size_t p = 0; p < in->n;) {
        size_t j = p;
        while (j < in->n && er_is_space(in->cp[j])) ++j;
        if (j < in->n && is_open(in->cp[j])) {
            size_t k = j + 1;
            while (k < in->n && !is_open(in->cp[k]) && !is_close(in->cp[k])) ++k;
            if (k < in->n && is_close(in->cp[k])) {
                ErText group = {0};
                text_slice(&group, in, j + 1, k);
                if (!noise_search(&group)) text_append(&a, in, p, k + 1);
                er_text_free(&group);
                p = k + 1;
                continue;
            }
        }
        er_text_push(&a, in->cp[p++]);
    }
    /* 2. \s*\bEXPLICIT VERSION\b -> "" */
    for (size_t p = 0; p < a.n;) {
        size_t j = p;
        while (j < a.n && er_is_space(a.cp[j])) ++j;
        if (boundary(&a, j) && match_ci(&a, j, "explicit version") && boundary(&a, j + 16)) {
            p = j + 16;
            continue;
        }
        er_text_push(&b, a.cp[p++]);
    }
    /* 3. [\(\[]\s*(?:feat\.?|ft\.?|featuring)\s+([^\)\]]*)[\)\]] -> "ft. \1" */
    for (size_t p = 0; p < b.n;) {
        if (is_open(b.cp[p])) {
            size_t j = p + 1;
            while (j < b.n && er_is_space(b.cp[j])) ++j;
            size_t e = feat_at(&b, j);
            if (e) {
                size_t k = e;
                while (k < b.n && !is_close(b.cp[k])) ++k;
                if (k < b.n) {
                    text_append_ascii(&c, "ft. ");
                    text_append(&c, &b, e, k);
                    p = k + 1;
                    continue;
                }
            }
        }
        er_text_push(&c, b.cp[p++]);
    }
    /* 4. FEAT -> " ft. " */
    a.n = 0;
    for (size_t p = 0; p < c.n;) {
        size_t s, e;
        if (!feat_search(&c, p, &s, &e)) {
            text_append(&a, &c, p, c.n);
            break;
        }
        text_append(&a, &c, p, s);
        text_append_ascii(&a, " ft. ");
        p = e;
    }
    /* 5. the same "ft. X" twice: parts = split(" ft. "), stripped; keep the first and every new non-empty one */
    ErText *parts = NULL;
    size_t count = 0, capacity = 0;
    for (size_t p = 0;;) {
        size_t s = find_literal(&a, p, " ft. ");
        if (count == capacity) {
            size_t grown = capacity ? capacity * 2 : 8;
            ErText *more = er_realloc(parts, capacity * sizeof(ErText), grown * sizeof(ErText));
            if (!more) break;
            parts = more;
            capacity = grown;
        }
        memset(&parts[count], 0, sizeof(ErText));
        text_slice(&parts[count], &a, p, s);
        text_strip(&parts[count]);
        ++count;
        if (s >= a.n) break;
        p = s + 5;
    }
    out->n = 0;
    ErText low = {0}, other = {0};
    for (size_t i = 0; i < count; ++i) {
        int keep = i == 0;
        if (i > 0 && parts[i].n) {
            keep = 1;
            er_text_lower(&parts[i], &low);
            for (size_t q = 1; q < i && keep; ++q) {
                er_text_lower(&parts[q], &other);
                if (text_equal(&low, &other)) keep = 0;
            }
        }
        if (!keep) continue;
        if (i > 0) text_append_ascii(out, " ft. ");
        text_append(out, &parts[i], 0, parts[i].n);
    }
    /* .strip(" -") */
    size_t s = 0, e = out->n;
    while (s < e && (out->cp[s] == ' ' || out->cp[s] == '-')) ++s;
    while (e > s && (out->cp[e - 1] == ' ' || out->cp[e - 1] == '-')) --e;
    if (s) memmove(out->cp, out->cp + s, (e - s) * sizeof(uint32_t));
    out->n = e - s;
    for (size_t i = 0; i < count; ++i) er_text_free(&parts[i]);
    er_free(parts);
    er_text_free(&low);
    er_text_free(&other);
    er_text_free(&a);
    er_text_free(&b);
    er_text_free(&c);
}

/* uploader_artist: name.strip() without a trailing VEVO|TV|Official|Music (re.I), stripped, or name.strip() */
static void uploader_artist(const ErText *name, ErText *out) {
    static const char *const suffixes[] = {"vevo", "tv", "official", "music"};
    ErText stripped = {0};
    text_copy(&stripped, name);
    text_strip(&stripped);
    size_t cut = stripped.n;
    for (size_t k = 0; k < 4; ++k) {
        size_t length = er_strlen(suffixes[k]);
        if (stripped.n >= length && match_ci(&stripped, stripped.n - length, suffixes[k]) && stripped.n - length < cut)
            cut = stripped.n - length;
    }
    text_slice(out, &stripped, 0, cut);
    text_strip(out);
    if (!out->n) text_copy(out, &stripped);
    er_text_free(&stripped);
}

static void split_artist_title(const ErText *text, const ErText *fallback, ErText *artist, ErText *title) {
    static const char *const separators[] = {" - ", ": "};
    for (size_t k = 0; k < 2; ++k) {
        size_t at = find_literal(text, 0, separators[k]);
        if (at < text->n) {
            text_slice(artist, text, 0, at);
            text_slice(title, text, at + er_strlen(separators[k]), text->n);
            text_strip(artist);
            text_strip(title);
            return;
        }
    }
    text_copy(artist, fallback);
    text_copy(title, text);
    text_strip(title);
}

static int has_ascii_letter(const ErBuf *b) {
    for (size_t i = 0; i < b->length; ++i)
        if ((b->data[i] >= 'A' && b->data[i] <= 'Z') || (b->data[i] >= 'a' && b->data[i] <= 'z')) return 1;
    return 0;
}
static int starts_with_ci(const ErBuf *text, const ErBuf *prefix) {
    if (prefix->length > text->length) return 0;
    for (size_t i = 0; i < prefix->length; ++i)
        if (lower_ascii((unsigned char)text->data[i]) != lower_ascii((unsigned char)prefix->data[i])) return 0;
    return 1;
}

static void ellipsize(ErBuf *text, size_t limit) {
    if (text->length <= limit) return;
    size_t n = limit - 3;
    while (n && (text->data[n - 1] == ' ' || text->data[n - 1] == '\t')) --n; /* rstrip (ASCII by now) */
    text->length = n;
    er_buf_str(text, "...");
}

void er_hud_names(const ErText *stem, const ErText *title_tag, const ErText *artist_tag, ErBuf *artist_out, ErBuf *title_out) {
    ErText artist = {0}, title = {0}, uploader = {0}, temp = {0};
    ErBuf folded = {0}, folded_uploader = {0};
    if (title_tag->n) {
        if (artist_tag->n) uploader_artist(artist_tag, &uploader);
        split_artist_title(title_tag, &uploader, &artist, &title);
        /* "98 Degrees" uploads "98º - ...": an artist without letters that the uploader name starts with is the uploader's */
        ascii_of(&artist, &folded);
        if (uploader.n && !has_ascii_letter(&folded)) {
            ascii_of(&uploader, &folded_uploader);
            if (starts_with_ci(&folded_uploader, &folded)) text_copy(&artist, &uploader);
        }
    } else {
        /* stem.split(" - "): three or more parts drop the uploader */
        size_t starts[64], ends[64], count = 0;
        for (size_t p = 0; count < 64;) {
            size_t s = find_literal(stem, p, " - ");
            starts[count] = p;
            ends[count] = s;
            ++count;
            if (s >= stem->n) break;
            p = s + 3;
        }
        size_t first = count >= 3 ? 1 : 0;
        if (count - first >= 2) {
            text_slice(&artist, stem, starts[first], ends[first]);
            text_slice(&title, stem, starts[first + 1], stem->n);
        } else {
            text_copy(&artist, artist_tag);
            text_slice(&title, stem, starts[first], ends[first]);
        }
    }
    /* a featured artist in the artist field moves to the title */
    for (size_t i = 0; i < artist.n; ++i)
        if (is_open(artist.cp[i]) || is_close(artist.cp[i])) artist.cp[i] = ' ';
    ErText featured = {0};
    size_t s, e;
    int has_feat = feat_search(&artist, 0, &s, &e);
    if (has_feat) {
        text_slice(&featured, &artist, e, artist.n);
        artist.n = s;
    }
    text_strip(&artist);
    er_strip_noise(&title, &temp);
    text_copy(&title, &temp);
    if (has_feat) {
        text_strip(&featured);
        ErText low_featured = {0}, low_title = {0};
        er_text_lower(&featured, &low_featured);
        er_text_lower(&title, &low_title);
        if (!text_contains(&low_title, &low_featured)) {
            text_append_ascii(&title, " ft. ");
            text_append(&title, &featured, 0, featured.n);
        }
        er_text_free(&low_featured);
        er_text_free(&low_title);
    }
    artist_out->length = 0;
    er_ascii_text(&artist, artist_out);
    if (!artist_out->length) er_buf_str(artist_out, "Unknown Artist");
    title_out->length = 0;
    er_ascii_text(&title, title_out);
    if (!title_out->length) er_ascii_text(stem, title_out);
    if (!title_out->length) er_buf_str(title_out, "Untitled");
    ellipsize(artist_out, ER_ARTIST_CHARS);
    ellipsize(title_out, ER_TITLE_CHARS);
    er_buf_cstr(artist_out);
    er_buf_cstr(title_out);
    er_text_free(&featured);
    er_text_free(&artist);
    er_text_free(&title);
    er_text_free(&uploader);
    er_text_free(&temp);
    er_buf_free(&folded);
    er_buf_free(&folded_uploader);
}
