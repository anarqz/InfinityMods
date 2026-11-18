/* EasyRadio core: a player's image -> the HUD radio icon (HUD_RadioIcon_<icon>.swatchbin).

   The format, the Xbox One tiled block order, premultiplied alpha, the BC7 modes 6/7 encoder and the decode-back
   check are ported from the Python prototype's logo_swatch.py and bc7.py, which adapt FH Mods Collection's
   spotify-radio/hud_logo (GPL-3.0-or-later; see NOTICE). The image is decoded with stb_image (public domain/MIT,
   third_party/). A 196x96 logo sits on a 256x128 BC7 surface (opaque black outside the logo area, as the stock
   icons), after the 0x53-byte header that all seven stock icons share: 32851 bytes in all. */
#include "er_core.h"

#include <string.h>

/* ---- stb_image, PNG/JPEG/BMP only, our allocator, no CRT I/O ---- */
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_THREAD_LOCALS
#define STBI_NO_FAILURE_STRINGS
#define STBI_NO_SIMD
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_PNM /* stb 2.30 needs PNM, HDR or linear for its size checks */
#define STBI_MAX_DIMENSIONS 8192
#define STBI_ASSERT(x) ((void)0)
#define STBI_MALLOC(n) er_alloc(n)
#define STBI_FREE(p) er_free(p)
#define STBI_REALLOC_SIZED(p, old, size) er_realloc(p, old, size)
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wimplicit-fallthrough"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#endif
#include "../third_party/stb_image.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#define HEADER_SIZE 0x53
#define SURFACE_W 256
#define SURFACE_H 128
#define LOGO_W 196
#define LOGO_H 96
#define BLOCKS_W (SURFACE_W / 4)
#define BLOCKS_H (SURFACE_H / 4)

/* the stock header (Grub/TXCB/TXCH, 256x128 BC7 surface, 196x96 logo); from logo_swatch.py KNOWN_HEADER */
static const uint8_t known_header[HEADER_SIZE] = {
    0x62, 0x75, 0x72, 0x47, 0x01, 0x00, 0x01, 0x00, 0x53, 0x00, 0x00, 0x00, 0x53, 0x80, 0x00, 0x00, 0x42, 0x43, 0x58, 0x54, 0x02,
    0x00, 0x01, 0x00, 0x28, 0x00, 0x00, 0x00, 0x53, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x48, 0x43,
    0x58, 0x54, 0x30, 0x02, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0xc4, 0x00, 0x60, 0x00, 0x01, 0x00, 0xc4, 0x00, 0x60, 0x00, 0x01, 0x00, 0x01, 0x01, 0x00, 0x2e, 0x01, 0x00, 0xb8,
};

/* ---- small float math (no libm in the plugin) ---- */
static double sqrt_d(double x) {
    __asm__("sqrtsd %1, %0" : "=x"(x) : "x"(x));
    return x;
}
static float absf(float x) {
    return x < 0 ? -x : x;
}
static float clampf(float x, float lo, float hi) {
    return x < lo ? lo : x > hi ? hi : x;
}
/* round half to even, like numpy */
static float roundf_even(float x) {
    float f = (float)(long long)x;
    if (x < 0 && f != x) f -= 1; /* floor */
    float d = x - f;
    if (d > 0.5f) return f + 1;
    if (d < 0.5f) return f;
    return ((long long)f % 2 == 0) ? f : f + 1;
}
static double sin_d(double x) {
    const double pi = 3.14159265358979323846;
    long long k = (long long)(x / (2 * pi));
    x -= (double)k * 2 * pi;
    if (x > pi) x -= 2 * pi;
    if (x < -pi) x += 2 * pi;
    if (x > pi / 2) x = pi - x;
    if (x < -pi / 2) x = -pi - x;
    double x2 = x * x, term = x, sum = x;
    for (int n = 1; n < 10; ++n) {
        term *= -x2 / (double)((2 * n) * (2 * n + 1));
        sum += term;
    }
    return sum;
}
static double lanczos3(double x) {
    const double pi = 3.14159265358979323846;
    if (x < 0) x = -x;
    if (x < 1e-9) return 1.0;
    if (x >= 3.0) return 0.0;
    return 3.0 * sin_d(pi * x) * sin_d(pi * x / 3.0) / (pi * pi * x * x);
}

/* separable Lanczos-3 resample of a float image with c channels */
static float *resample_axis(const float *in, int w, int h, int c, int out_w, int out_h, int horizontal) {
    float *out = er_alloc((size_t)out_w * (size_t)out_h * (size_t)c * sizeof(float));
    if (!out) return NULL;
    int in_n = horizontal ? w : h, out_n = horizontal ? out_w : out_h, lines = horizontal ? h : w;
    double scale = (double)in_n / (double)out_n, filter_scale = scale > 1.0 ? scale : 1.0, support = 3.0 * filter_scale;
    for (int o = 0; o < out_n; ++o) {
        double center = (o + 0.5) * scale;
        int first = (int)(center - support - 1), last = (int)(center + support + 1);
        if (first < 0) first = 0;
        if (last > in_n - 1) last = in_n - 1;
        double total = 0;
        for (int i = first; i <= last; ++i) total += lanczos3((i + 0.5 - center) / filter_scale);
        for (int line = 0; line < lines; ++line) {
            double acc[4] = {0, 0, 0, 0};
            for (int i = first; i <= last; ++i) {
                double weight = lanczos3((i + 0.5 - center) / filter_scale);
                const float *px = horizontal ? in + ((size_t)line * (size_t)w + (size_t)i) * (size_t)c : in + ((size_t)i * (size_t)w + (size_t)line) * (size_t)c;
                for (int k = 0; k < c; ++k) acc[k] += weight * px[k];
            }
            float *dst = horizontal ? out + ((size_t)line * (size_t)out_w + (size_t)o) * (size_t)c : out + ((size_t)o * (size_t)out_w + (size_t)line) * (size_t)c;
            for (int k = 0; k < c; ++k) dst[k] = (float)(total != 0 ? acc[k] / total : 0);
        }
    }
    return out;
}

/* ---- BC7 modes 6 and 7 (bc7.py) ---- */
static const int w6[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
static const int w7[4] = {0, 21, 43, 64};
static const uint16_t partitions2[64] = {
    0xcccc, 0x8888, 0xeeee, 0xecc8, 0xc880, 0xfeec, 0xfec8, 0xec80, 0xc800, 0xffec, 0xfe80, 0xe800, 0xffe8, 0xff00, 0xfff0, 0xf000,
    0xf710, 0x008e, 0x7100, 0x08ce, 0x008c, 0x7310, 0x3100, 0x8cce, 0x088c, 0x3110, 0x6666, 0x366c, 0x17e8, 0x0ff0, 0x718e, 0x399c,
    0xaaaa, 0xf0f0, 0x5a5a, 0x33cc, 0x3c3c, 0x55aa, 0x9696, 0xa55a, 0x73ce, 0x13c8, 0x324c, 0x3bdc, 0x6996, 0xc33c, 0x9966, 0x0660,
    0x0272, 0x04e4, 0x4e40, 0x2720, 0xc936, 0x936c, 0x39c6, 0x639c, 0x9336, 0x9cc6, 0x817e, 0xe718, 0xccf0, 0x0fcc, 0x7744, 0xee22};
static const uint8_t anchor2[64] = {15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2,  8, 2,  2,  8,
                                    8,  15, 2,  8,  2,  2,  8,  8,  2,  2,  15, 15, 6,  8,  2,  8,  15, 15, 2,  8,  2,  2,
                                    2,  15, 15, 6,  6,  2,  6,  8,  15, 15, 2,  2,  15, 15, 15, 15, 15, 2,  2,  15};

typedef struct {
    uint8_t bytes[16];
    int bit;
} Bits;
static void put(Bits *b, unsigned value, int width) {
    for (int i = 0; i < width; ++i, ++b->bit)
        if ((value >> i) & 1) b->bytes[b->bit >> 3] |= (uint8_t)(1u << (b->bit & 7));
}
static unsigned get(const uint8_t *bytes, int *bit, int width) {
    unsigned v = 0;
    for (int i = 0; i < width; ++i, ++*bit) v |= (unsigned)((bytes[*bit >> 3] >> (*bit & 7)) & 1) << i;
    return v;
}

/* principal axis of n RGBA points (power iteration on the covariance); returns 0 for a flat set */
static void principal(const float (*px)[4], int n, float mean[4], float axis[4]) {
    for (int c = 0; c < 4; ++c) {
        mean[c] = 0;
        for (int i = 0; i < n; ++i) mean[c] += px[i][c];
        mean[c] /= (float)n;
    }
    double cov[4][4] = {{0}};
    double sum = 0;
    for (int i = 0; i < n; ++i)
        for (int a = 0; a < 4; ++a)
            for (int b = 0; b < 4; ++b) cov[a][b] += (double)(px[i][a] - mean[a]) * (px[i][b] - mean[b]);
    for (int a = 0; a < 4; ++a)
        for (int b = 0; b < 4; ++b) sum += cov[a][b] < 0 ? -cov[a][b] : cov[a][b];
    if (sum <= 1e-6) {
        for (int c = 0; c < 4; ++c) axis[c] = 0.5f;
        return;
    }
    double v[4] = {1, 1, 1, 1};
    int big = 0;
    for (int a = 1; a < 4; ++a)
        if (cov[a][a] > cov[big][big]) big = a;
    v[big] = 2;
    for (int iteration = 0; iteration < 32; ++iteration) {
        double next[4] = {0, 0, 0, 0}, norm = 0;
        for (int a = 0; a < 4; ++a)
            for (int b = 0; b < 4; ++b) next[a] += cov[a][b] * v[b];
        for (int a = 0; a < 4; ++a) norm += next[a] * next[a];
        if (norm <= 1e-30) break;
        double inv = 1.0 / sqrt_d(norm);
        for (int a = 0; a < 4; ++a) v[a] = next[a] * inv;
    }
    for (int c = 0; c < 4; ++c) axis[c] = (float)v[c];
}

/* the endpoints along the axis, then up to 3 rounds of quantise / index / least-squares refit */
typedef struct {
    float error;
    int q[2][4], p[2];
    int index[16];
} Fit;
static void quantize(const float e[4], int bits, int out_q[4], int *out_p, int expanded[4]) {
    float best = -1;
    for (int p = 0; p < 2; ++p) {
        int q[4], x[4];
        float err = 0;
        for (int c = 0; c < 4; ++c) {
            if (bits == 7) {
                q[c] = (int)clampf(roundf_even((e[c] - (float)p) / 2), 0, 127);
                x[c] = (q[c] << 1) | p;
            } else {
                q[c] = (int)clampf(roundf_even((e[c] * 63.0f / 255.0f - (float)p) / 2), 0, 31);
                int e6 = (q[c] << 1) | p;
                x[c] = (e6 << 2) | (e6 >> 4);
            }
            err += absf((float)x[c] - e[c]);
        }
        if (best < 0 || err < best) {
            best = err;
            for (int c = 0; c < 4; ++c) out_q[c] = q[c], expanded[c] = x[c];
            *out_p = p;
        }
    }
}
static void fit_points(const float (*px)[4], int n, int mode, Fit *best) {
    const int *weights = mode == 6 ? w6 : w7;
    int levels = mode == 6 ? 16 : 4;
    float mean[4], axis[4];
    principal(px, n, mean, axis);
    float tmin = 1e30f, tmax = -1e30f;
    for (int i = 0; i < n; ++i) {
        float t = 0;
        for (int c = 0; c < 4; ++c) t += (px[i][c] - mean[c]) * axis[c];
        if (t < tmin) tmin = t;
        if (t > tmax) tmax = t;
    }
    float lo[4], hi[4];
    for (int c = 0; c < 4; ++c) lo[c] = mean[c] + axis[c] * tmin, hi[c] = mean[c] + axis[c] * tmax;
    best->error = -1;
    for (int round = 0; round < 3; ++round) {
        Fit fit;
        int e0[4], e1[4], pal[16][4];
        float clo[4], chi[4];
        for (int c = 0; c < 4; ++c) clo[c] = clampf(lo[c], 0, 255), chi[c] = clampf(hi[c], 0, 255);
        quantize(clo, mode == 6 ? 7 : 5, fit.q[0], &fit.p[0], e0);
        quantize(chi, mode == 6 ? 7 : 5, fit.q[1], &fit.p[1], e1);
        for (int k = 0; k < levels; ++k)
            for (int c = 0; c < 4; ++c) pal[k][c] = ((64 - weights[k]) * e0[c] + weights[k] * e1[c] + 32) >> 6;
        fit.error = 0;
        for (int i = 0; i < n; ++i) {
            float best_d = -1;
            int best_k = 0;
            for (int k = 0; k < levels; ++k) {
                float d = 0;
                for (int c = 0; c < 4; ++c) d += absf(px[i][c] - (float)pal[k][c]);
                if (best_d < 0 || d < best_d) best_d = d, best_k = k;
            }
            fit.index[i] = best_k;
            fit.error += best_d;
        }
        if (best->error < 0 || fit.error < best->error) *best = fit;
        /* least squares: px ~ (1-w) lo + w hi */
        double a = 0, b = 0, d = 0, r0[4] = {0}, r1[4] = {0};
        for (int i = 0; i < n; ++i) {
            double w = weights[fit.index[i]] / 64.0;
            a += (1 - w) * (1 - w), b += (1 - w) * w, d += w * w;
            for (int c = 0; c < 4; ++c) r0[c] += (1 - w) * px[i][c], r1[c] += w * px[i][c];
        }
        double det = a * d - b * b;
        if (det < 1e-9 * (a + d) * (a + d) || det <= 0) break;
        for (int c = 0; c < 4; ++c) {
            lo[c] = (float)((d * r0[c] - b * r1[c]) / det);
            hi[c] = (float)((a * r1[c] - b * r0[c]) / det);
        }
    }
}

static float encode_block6(float px[16][4], uint8_t out[16]) {
    int transparent = 1;
    for (int i = 0; i < 16; ++i)
        if (px[i][3] != 0) transparent = 0;
    if (transparent)
        for (int i = 0; i < 16; ++i) px[i][0] = px[i][1] = px[i][2] = px[i][3] = 0;
    Fit f;
    fit_points((const float(*)[4])px, 16, 6, &f);
    if (f.index[0] >= 8) {
        for (int c = 0; c < 4; ++c) {
            int t = f.q[0][c];
            f.q[0][c] = f.q[1][c];
            f.q[1][c] = t;
        }
        int t = f.p[0];
        f.p[0] = f.p[1];
        f.p[1] = t;
        for (int i = 0; i < 16; ++i) f.index[i] = 15 - f.index[i];
    }
    Bits b = {{0}, 0};
    put(&b, 1u << 6, 7);
    for (int c = 0; c < 4; ++c) put(&b, (unsigned)f.q[0][c], 7), put(&b, (unsigned)f.q[1][c], 7);
    put(&b, (unsigned)f.p[0], 1);
    put(&b, (unsigned)f.p[1], 1);
    put(&b, (unsigned)f.index[0], 3);
    for (int i = 1; i < 16; ++i) put(&b, (unsigned)f.index[i], 4);
    memcpy(out, b.bytes, 16);
    return f.error;
}

static float encode_block7(float px[16][4], uint8_t out[16]) {
    float best_error = -1;
    int best_part = 0;
    Fit best_fit[2];
    for (int part = 0; part < 64; ++part) {
        float subset_px[2][16][4];
        int count[2] = {0, 0};
        for (int i = 0; i < 16; ++i) {
            int s = (partitions2[part] >> i) & 1;
            memcpy(subset_px[s][count[s]++], px[i], sizeof(px[i]));
        }
        Fit fits[2];
        fit_points((const float(*)[4])subset_px[0], count[0], 7, &fits[0]);
        fit_points((const float(*)[4])subset_px[1], count[1], 7, &fits[1]);
        float error = fits[0].error + fits[1].error;
        if (best_error < 0 || error < best_error) {
            best_error = error;
            best_part = part;
            best_fit[0] = fits[0];
            best_fit[1] = fits[1];
        }
    }
    int indices[16];
    for (int s = 0; s < 2; ++s) {
        int anchor = s ? anchor2[best_part] : 0, k = 0, where[16], n = 0;
        for (int i = 0; i < 16; ++i)
            if (((partitions2[best_part] >> i) & 1) == s) where[n++] = i;
        Fit *f = &best_fit[s];
        for (k = 0; k < n && where[k] != anchor; ++k) {
        }
        if (k < n && f->index[k] >= 2) {
            for (int c = 0; c < 4; ++c) {
                int t = f->q[0][c];
                f->q[0][c] = f->q[1][c];
                f->q[1][c] = t;
            }
            int t = f->p[0];
            f->p[0] = f->p[1];
            f->p[1] = t;
            for (int i = 0; i < n; ++i) f->index[i] = 3 - f->index[i];
        }
        for (int i = 0; i < n; ++i) indices[where[i]] = f->index[i];
    }
    Bits b = {{0}, 0};
    put(&b, 1u << 7, 8);
    put(&b, (unsigned)best_part, 6);
    for (int c = 0; c < 4; ++c)
        for (int s = 0; s < 2; ++s) put(&b, (unsigned)best_fit[s].q[0][c], 5), put(&b, (unsigned)best_fit[s].q[1][c], 5);
    for (int s = 0; s < 2; ++s) put(&b, (unsigned)best_fit[s].p[0], 1), put(&b, (unsigned)best_fit[s].p[1], 1);
    for (int i = 0; i < 16; ++i) put(&b, (unsigned)indices[i], (i == 0 || i == anchor2[best_part]) ? 1 : 2);
    memcpy(out, b.bytes, 16);
    return best_error;
}

/* decodes a mode 6 or 7 block into 16 RGBA pixels; 0 for any other mode */
static int decode_block(const uint8_t in[16], uint8_t rgba[16][4]) {
    int bit = 0;
    if ((in[0] & 0x7f) == 0x40) { /* mode 6 */
        bit = 7;
        int e[2][4];
        for (int c = 0; c < 4; ++c) e[0][c] = (int)get(in, &bit, 7), e[1][c] = (int)get(in, &bit, 7);
        int p0 = (int)get(in, &bit, 1), p1 = (int)get(in, &bit, 1);
        for (int c = 0; c < 4; ++c) e[0][c] = (e[0][c] << 1) | p0, e[1][c] = (e[1][c] << 1) | p1;
        for (int i = 0; i < 16; ++i) {
            int idx = (int)get(in, &bit, i == 0 ? 3 : 4);
            for (int c = 0; c < 4; ++c) rgba[i][c] = (uint8_t)(((64 - w6[idx]) * e[0][c] + w6[idx] * e[1][c] + 32) >> 6);
        }
        return 1;
    }
    if (in[0] == 0x80) { /* mode 7 */
        bit = 8;
        int part = (int)get(in, &bit, 6), e[2][2][4], pb[2][2];
        for (int c = 0; c < 4; ++c)
            for (int s = 0; s < 2; ++s) e[s][0][c] = (int)get(in, &bit, 5), e[s][1][c] = (int)get(in, &bit, 5);
        for (int s = 0; s < 2; ++s) pb[s][0] = (int)get(in, &bit, 1), pb[s][1] = (int)get(in, &bit, 1);
        for (int s = 0; s < 2; ++s)
            for (int j = 0; j < 2; ++j)
                for (int c = 0; c < 4; ++c) {
                    int e6 = (e[s][j][c] << 1) | pb[s][j];
                    e[s][j][c] = (e6 << 2) | (e6 >> 4);
                }
        for (int i = 0; i < 16; ++i) {
            int s = (partitions2[part] >> i) & 1;
            int idx = (int)get(in, &bit, (i == 0 || i == anchor2[part]) ? 1 : 2);
            for (int c = 0; c < 4; ++c) rgba[i][c] = (uint8_t)(((64 - w7[idx]) * e[s][0][c] + w7[idx] * e[s][1][c] + 32) >> 6);
        }
        return 1;
    }
    return 0;
}

/* ---- the tiled block order (logo_swatch.py block_xy) ---- */
static void block_xy(int i, int *bx, int *by) {
    int x = (i & 1) | ((i >> 1) & 2);
    int y = ((i >> 1) & 1) | ((i >> 2) & 2);
    int p[7];
    for (int b = 0; b < 7; ++b) p[b] = (i >> (4 + b)) & 1;
    x |= (p[5] << 2) | ((p[0] ^ p[1] ^ p[3] ^ p[4]) << 3) | ((p[1] ^ p[3]) << 4) | (p[2] << 5);
    y |= (p[6] << 2) | (p[4] << 3) | (p[3] << 4);
    *bx = x;
    *by = y;
}

int er_swatch_decode(const uint8_t *swatch, uint8_t *rgba) {
    if (memcmp(swatch, known_header, HEADER_SIZE)) return 0;
    for (int i = 0; i < BLOCKS_W * BLOCKS_H; ++i) {
        int bx, by;
        block_xy(i, &bx, &by);
        uint8_t px[16][4];
        if (!decode_block(swatch + HEADER_SIZE + i * 16, px)) return 0;
        for (int k = 0; k < 16; ++k) memcpy(rgba + (((size_t)(by * 4 + k / 4) * SURFACE_W) + (size_t)(bx * 4 + k % 4)) * 4, px[k], 4);
    }
    return 1;
}

static void say(char *why, size_t size, const char *text) {
    size_t i = 0;
    for (; text[i] && i + 1 < size; ++i) why[i] = text[i];
    if (size) why[i] = 0;
}

int er_logo_swatch(const uint8_t *image, size_t size, uint8_t *out, char *why, size_t why_size) {
    int w = 0, h = 0, channels = 0;
    if (size > 0x7fffffff) {
        say(why, why_size, "image file too large");
        return 0;
    }
    unsigned char *pixels = stbi_load_from_memory(image, (int)size, &w, &h, &channels, 4);
    if (!pixels || w <= 0 || h <= 0) {
        say(why, why_size, "not a PNG, JPEG or BMP image stb_image can read");
        if (pixels) stbi_image_free(pixels);
        return 0;
    }
    /* premultiply before scaling: no dark fringes */
    float *pm = er_alloc((size_t)w * (size_t)h * 4 * sizeof(float));
    if (!pm) {
        stbi_image_free(pixels);
        say(why, why_size, "out of memory");
        return 0;
    }
    for (size_t i = 0; i < (size_t)w * (size_t)h; ++i) {
        float a = pixels[i * 4 + 3];
        for (int c = 0; c < 3; ++c) pm[i * 4 + (size_t)c] = pixels[i * 4 + (size_t)c] * a / 255.0f;
        pm[i * 4 + 3] = a;
    }
    stbi_image_free(pixels);
    double scale = (double)LOGO_W / w < (double)LOGO_H / h ? (double)LOGO_W / w : (double)LOGO_H / h;
    int fw = (int)(w * scale + 0.5), fh = (int)(h * scale + 0.5);
    if (fw < 1) fw = 1;
    if (fh < 1) fh = 1;
    if (fw > LOGO_W) fw = LOGO_W;
    if (fh > LOGO_H) fh = LOGO_H;
    float *across = resample_axis(pm, w, h, 4, fw, h, 1);
    er_free(pm);
    float *fitted = across ? resample_axis(across, fw, h, 4, fw, fh, 0) : NULL;
    er_free(across);
    float *surface = er_alloc(SURFACE_W * SURFACE_H * 4 * sizeof(float));
    uint8_t *source = er_alloc(SURFACE_W * SURFACE_H * 4);
    uint8_t *back = er_alloc(SURFACE_W * SURFACE_H * 4);
    int ok = 0;
    if (!fitted || !surface || !source || !back) {
        say(why, why_size, "out of memory");
        goto done;
    }
    for (int i = 0; i < SURFACE_W * SURFACE_H; ++i) surface[i * 4 + 3] = 255; /* opaque black outside the logo area */
    for (int y = 0; y < LOGO_H; ++y)
        for (int x = 0; x < LOGO_W; ++x)
            for (int c = 0; c < 4; ++c) surface[((size_t)y * SURFACE_W + (size_t)x) * 4 + (size_t)c] = 0;
    int ox = (LOGO_W - fw) / 2, oy = (LOGO_H - fh) / 2;
    for (int y = 0; y < fh; ++y)
        for (int x = 0; x < fw; ++x) {
            float *src = fitted + ((size_t)y * (size_t)fw + (size_t)x) * 4;
            float *dst = surface + ((size_t)(y + oy) * SURFACE_W + (size_t)(x + ox)) * 4;
            float a = clampf(src[3], 0, 255);
            for (int c = 0; c < 3; ++c) {
                float v = clampf(src[c], 0, 255);
                dst[c] = v < a ? v : a;
            }
            dst[3] = a;
        }
    for (int i = 0; i < SURFACE_W * SURFACE_H * 4; ++i) source[i] = (uint8_t)roundf_even(surface[i]);
    memcpy(out, known_header, HEADER_SIZE);
    for (int i = 0; i < BLOCKS_W * BLOCKS_H; ++i) {
        int bx, by;
        block_xy(i, &bx, &by);
        float px[16][4];
        for (int k = 0; k < 16; ++k)
            for (int c = 0; c < 4; ++c) px[k][c] = source[(((size_t)(by * 4 + k / 4) * SURFACE_W) + (size_t)(bx * 4 + k % 4)) * 4 + (size_t)c];
        uint8_t *block = out + HEADER_SIZE + i * 16;
        float error = encode_block6(px, block);
        if (error > 16 * 4 * 4) { /* more than ~4 per channel off: try two subsets */
            float again[16][4];
            for (int k = 0; k < 16; ++k)
                for (int c = 0; c < 4; ++c) again[k][c] = source[(((size_t)(by * 4 + k / 4) * SURFACE_W) + (size_t)(bx * 4 + k % 4)) * 4 + (size_t)c];
            uint8_t block7[16];
            if (encode_block7(again, block7) < error) memcpy(block, block7, 16);
        }
    }
    /* decode back: visible error over a grey background, and no colour brighter than its alpha */
    if (!er_swatch_decode(out, back)) {
        say(why, why_size, "encoding check failed: cannot decode it back");
        goto done;
    }
    int max_error = 0;
    unsigned brighter = 0;
    for (int y = 0; y < LOGO_H; ++y)
        for (int x = 0; x < LOGO_W; ++x) {
            const uint8_t *s = source + ((size_t)y * SURFACE_W + (size_t)x) * 4, *b = back + ((size_t)y * SURFACE_W + (size_t)x) * 4;
            for (int c = 0; c < 3; ++c) {
                float over_s = s[c] + 120.0f * (1 - s[3] / 255.0f), over_b = b[c] + 120.0f * (1 - b[3] / 255.0f);
                int e = (int)absf(over_s - over_b);
                if (e > max_error) max_error = e;
            }
            int m = b[0] > b[1] ? b[0] : b[1];
            if (b[2] > m) m = b[2];
            if (m > b[3] + 6) ++brighter;
        }
    if (max_error > 64 || brighter * 1000 > LOGO_W * LOGO_H) {
        say(why, why_size, "encoding check failed (visible error or too-bright pixels)");
        goto done;
    }
    ok = 1;
done:
    er_free(fitted);
    er_free(surface);
    er_free(source);
    er_free(back);
    return ok;
}
