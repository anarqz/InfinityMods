/* EasyRadio core: buffers, formatting, strings, shuffle and sort. */
#include "er_core.h"

#include <string.h>

void *er_realloc(void *memory, size_t old_size, size_t new_size) {
    void *fresh = er_alloc(new_size);
    if (!fresh) return NULL;
    if (memory) {
        memcpy(fresh, memory, old_size < new_size ? old_size : new_size);
        er_free(memory);
    }
    return fresh;
}

static int buf_reserve(ErBuf *buf, size_t extra) {
    if (buf->failed) return 0;
    if (buf->length + extra + 1 <= buf->capacity) return 1;
    size_t capacity = buf->capacity ? buf->capacity : 256;
    while (capacity < buf->length + extra + 1) capacity *= 2;
    char *data = er_realloc(buf->data, buf->capacity, capacity);
    if (!data) {
        buf->failed = 1;
        return 0;
    }
    buf->data = data;
    buf->capacity = capacity;
    return 1;
}

void er_buf_bytes(ErBuf *buf, const void *data, size_t length) {
    if (!length || !buf_reserve(buf, length)) return;
    memcpy(buf->data + buf->length, data, length);
    buf->length += length;
    buf->data[buf->length] = 0;
}
void er_buf_str(ErBuf *buf, const char *text) {
    er_buf_bytes(buf, text, er_strlen(text));
}
void er_buf_char(ErBuf *buf, char c) {
    er_buf_bytes(buf, &c, 1);
}
static void buf_number(ErBuf *buf, uint64_t value, unsigned base) {
    char digits[24];
    int n = 0;
    do {
        digits[n++] = "0123456789abcdef"[value % base];
        value /= base;
    } while (value);
    while (n) er_buf_char(buf, digits[--n]);
}
void er_buf_uint(ErBuf *buf, uint64_t value) {
    buf_number(buf, value, 10);
}
void er_buf_free(ErBuf *buf) {
    er_free(buf->data);
    buf->data = NULL;
    buf->length = buf->capacity = 0;
    buf->failed = 0;
}
const char *er_buf_cstr(ErBuf *buf) {
    if (!buf_reserve(buf, 0)) return "";
    buf->data[buf->length] = 0;
    return buf->data;
}

void er_buf_vformat(ErBuf *buf, const char *format, va_list args) {
    for (const char *f = format; *f; ++f) {
        if (*f != '%' || !f[1]) {
            er_buf_char(buf, *f);
            continue;
        }
        ++f;
        int long_long = 0;
        if (f[0] == 'l' && f[1] == 'l') {
            long_long = 1;
            f += 2;
        }
        switch (*f) {
        case 's': {
            const char *s = va_arg(args, const char *);
            er_buf_str(buf, s ? s : "(null)");
            break;
        }
        case 'c': er_buf_char(buf, (char)va_arg(args, int)); break;
        case 'd': {
            long long v = long_long ? va_arg(args, long long) : va_arg(args, int);
            if (v < 0) {
                er_buf_char(buf, '-');
                buf_number(buf, (uint64_t)(-(v + 1)) + 1, 10);
            } else {
                buf_number(buf, (uint64_t)v, 10);
            }
            break;
        }
        case 'u': buf_number(buf, long_long ? va_arg(args, unsigned long long) : va_arg(args, unsigned), 10); break;
        case 'x': buf_number(buf, long_long ? va_arg(args, unsigned long long) : va_arg(args, unsigned), 16); break;
        case 'p':
            er_buf_str(buf, "0x");
            buf_number(buf, (uint64_t)(uintptr_t)va_arg(args, void *), 16);
            break;
        default: er_buf_char(buf, *f); break;
        }
    }
}
void er_buf_format(ErBuf *buf, const char *format, ...) {
    va_list args;
    va_start(args, format);
    er_buf_vformat(buf, format, args);
    va_end(args);
}
void er_log(const char *format, ...) {
    ErBuf line = {0};
    va_list args;
    va_start(args, format);
    er_buf_vformat(&line, format, args);
    va_end(args);
    er_log_line(er_buf_cstr(&line));
    er_buf_free(&line);
}

size_t er_strlen(const char *text) {
    size_t n = 0;
    while (text[n]) ++n;
    return n;
}
int er_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) ++a, ++b;
    return (unsigned char)*a - (unsigned char)*b;
}
char *er_strdup(const char *text, size_t length) {
    char *copy = er_alloc(length + 1);
    if (copy) memcpy(copy, text, length);
    return copy;
}
static int lower_ascii(int c) {
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}
int er_equal_ci(const char *a, size_t length, const char *b) {
    size_t i = 0;
    for (; i < length && b[i]; ++i)
        if (lower_ascii((unsigned char)a[i]) != lower_ascii((unsigned char)b[i])) return 0;
    return i == length && !b[i];
}
char *er_join(const char *a, const char *b, const char *c) {
    size_t la = er_strlen(a), lb = b ? er_strlen(b) : 0, lc = c ? er_strlen(c) : 0;
    char *out = er_alloc(la + lb + lc + 1);
    if (!out) return NULL;
    memcpy(out, a, la);
    if (lb) memcpy(out + la, b, lb);
    if (lc) memcpy(out + la + lb, c, lc);
    return out;
}

/* splitmix64: a fresh seed per game start gives a fresh order */
uint64_t er_random_next(ErRandom *random) {
    uint64_t z = (random->state += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}
static void swap_bytes(unsigned char *a, unsigned char *b, size_t size) {
    while (size--) {
        unsigned char t = *a;
        *a++ = *b;
        *b++ = t;
    }
}
/* Fisher-Yates, unbiased (rejection sampling) */
void er_shuffle(void *items, size_t count, size_t size, ErRandom *random) {
    unsigned char *base = items;
    for (size_t i = count; i > 1; --i) {
        uint64_t limit = UINT64_MAX - UINT64_MAX % i, r;
        do r = er_random_next(random);
        while (r >= limit);
        size_t j = (size_t)(r % i);
        if (j != i - 1) swap_bytes(base + j * size, base + (i - 1) * size, size);
    }
}
/* stable insertion-merge sort (small element counts; songs per folder) */
void er_sort(void *items, size_t count, size_t size, int (*less)(const void *, const void *)) {
    unsigned char *base = items;
    unsigned char *temp = er_alloc(size ? size : 1);
    if (!temp) return;
    for (size_t i = 1; i < count; ++i) {
        memcpy(temp, base + i * size, size);
        size_t j = i;
        while (j && less(temp, base + (j - 1) * size)) {
            memcpy(base + j * size, base + (j - 1) * size, size);
            --j;
        }
        memcpy(base + j * size, temp, size);
    }
    er_free(temp);
}
