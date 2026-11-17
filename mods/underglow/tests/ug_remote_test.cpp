// Host test of Underglow's remote-neon logic (ug_remote.h, added by patches/0002): the 40-byte blob, the light cap and
// the nearest-8-within-150 m choice. Built and run by run_tests.py against the patched upstream tree.
#include "ug_remote.h"

#include <cstdio>

static int g_fail = 0, g_checks = 0;
#define CHECK(x)                                                         \
    do {                                                                 \
        ++g_checks;                                                      \
        if (!(x)) {                                                      \
            ++g_fail;                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);      \
        }                                                                \
    } while (0)

static bool near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

static ugr::Style sample() {
    ugr::Style s;
    s.on = true;
    s.parts = 0x0B;   // front, right, left
    s.led = 3;
    s.rgb[0][0] = 0x00, s.rgb[0][1] = 0xFF, s.rgb[0][2] = 0x80;
    s.rgb[1][0] = 0xFF, s.rgb[1][1] = 0x1A, s.rgb[1][2] = 0x99;
    s.effect[0] = 2, s.effect[1] = 17, s.effect[2] = 0, s.effect[3] = 60;
    s.brightness = 0.25f;
    s.speed = 0.3f;
    s.side = 10, s.front = 6, s.rear = 6;
    s.height = 0.01f, s.front_inset = 0.5f, s.rear_inset = 0.7f, s.side_offset = -0.2f, s.side_trim = 0.8f;
    s.range = 1.0f, s.power = 4.0f, s.inner = 60.0f, s.outer = 170.0f;
    return s;
}

static void test_golden() {
    uint8_t b[ugr::kBlobSize];
    ugr::encode(sample(), b);
    const uint8_t want[ugr::kBlobSize] = {
        0x01, 0x0B, 0x03, 0x00,                     // flags, parts, led, 0
        0x00, 0xFF, 0x80, 0xFF, 0x1A, 0x99,         // primary, secondary
        0x02, 0x11, 0x00, 0x3C,                     // effects
        0x00, 0xFA, 0x01, 0x2C,                     // brightness 250, speed 300
        0x0A, 0x06, 0x06, 0x00,                     // side, front, rear, 0
        0x00, 0x0A, 0x01, 0xF4, 0x02, 0xBC,         // height 10, front inset 500, rear inset 700 (mm)
        0xFF, 0x38, 0x03, 0x20,                     // side offset -200, side trim 800
        0x00, 0x64, 0x01, 0x90,                     // range 100 cm, power 400
        0x3C, 0xAA, 0x00, 0x00,                     // inner 60, outer 170, 0, 0
    };
    CHECK(std::memcmp(b, want, sizeof want) == 0);
    for (int i = 0; i < ugr::kBlobSize; ++i)
        if (b[i] != want[i]) std::printf("  byte %d: %02X, want %02X\n", i, b[i], want[i]);
}

static void test_roundtrip() {
    uint8_t b[ugr::kBlobSize];
    const ugr::Style s = sample();
    ugr::encode(s, b);
    ugr::Style d;
    CHECK(ugr::decode(b, ugr::kBlobSize, 1, 61, &d));
    CHECK(d.on && d.parts == 0x0B && d.led == 3);
    CHECK(std::memcmp(d.rgb, s.rgb, sizeof d.rgb) == 0);
    CHECK(std::memcmp(d.effect, s.effect, sizeof d.effect) == 0);
    CHECK(near(d.brightness, 0.25f, 1e-4f) && near(d.speed, 0.3f, 1e-4f));
    CHECK(d.side == 10 && d.front == 6 && d.rear == 6);
    CHECK(near(d.height, 0.01f, 1e-4f) && near(d.front_inset, 0.5f, 1e-4f) && near(d.rear_inset, 0.7f, 1e-4f));
    CHECK(near(d.side_offset, -0.2f, 1e-4f) && near(d.side_trim, 0.8f, 1e-4f));
    CHECK(near(d.range, 1.0f, 1e-4f) && near(d.power, 4.0f, 1e-4f) && d.inner == 60.0f && d.outer == 170.0f);

    ugr::Style off;   // the "off" blob published at registration
    off.on = false;
    off.parts = 0;
    ugr::encode(off, b);
    CHECK(b[0] == 0 && b[1] == 0 && b[2] == 1);
    CHECK(ugr::decode(b, ugr::kBlobSize, 1, 61, &d) && !d.on && d.parts == 0);
}

static void test_refusals() {
    uint8_t good[ugr::kBlobSize], b[64];
    ugr::encode(sample(), good);
    ugr::Style d;
    auto with = [&](int at, uint8_t v) {
        std::memcpy(b, good, sizeof good);
        b[at] = v;
        return ugr::decode(b, ugr::kBlobSize, 1, 61, &d);
    };
    CHECK(!ugr::decode(nullptr, 40, 1, 61, &d));
    CHECK(!ugr::decode(good, 39, 1, 61, &d));   // short
    std::memcpy(b, good, 40);
    CHECK(!ugr::decode(b, 41, 1, 61, &d));      // v1 is exactly 40
    CHECK(!ugr::decode(good, 40, 0, 61, &d));   // version 0
    CHECK(!with(0, 2));                         // unknown flag
    CHECK(!with(1, 0x10));                      // part bit 4
    CHECK(!with(2, 0));                         // LED level 0
    CHECK(!with(2, 4));                         // LED level 4
    CHECK(!with(3, 1));                         // reserved
    CHECK(!with(21, 1));
    CHECK(!with(38, 1));
    CHECK(!with(39, 1));
    CHECK(!with(18, 65));                       // more than 64 on an edge
    CHECK(!with(36, 181));                      // cone past 180
    CHECK(!with(37, 200));
    CHECK(with(13, 61) && d.effect[3] == 0);    // an effect this build lacks plays Static
    CHECK(with(36, 180) && d.inner == 170.0f);  // inner never wider than outer
    // a later compatible layout: longer, reserved bytes not checked, the first 40 read the same
    std::memcpy(b, good, 40);
    std::memset(b + 40, 0xEE, 24);
    b[39] = 0x55;
    CHECK(ugr::decode(b, 64, 2, 61, &d) && d.side == 10 && d.led == 3);
    CHECK(!ugr::decode(b, 65, 2, 61, &d));
    // hostile numbers are held to what a light takes
    std::memcpy(b, good, 40);
    b[14] = b[15] = 0xFF;   // brightness 65.535
    b[16] = b[17] = 0xFF;   // speed
    b[32] = b[33] = 0x00;   // range 0
    b[34] = b[35] = 0xFF;   // power 655
    b[22] = 0x7F, b[23] = 0xFF;   // height 32.767 m
    CHECK(ugr::decode(b, 40, 1, 61, &d));
    CHECK(d.brightness == 4.0f && d.speed == 10.0f && d.range == 0.05f && d.power == 50.0f && d.height == 1.0f);
}

static void test_encode_clamps() {
    ugr::Style s = sample();
    s.led = 9;
    s.side = 200;
    s.parts = 0xFF;
    s.brightness = -1.0f;
    s.height = 100.0f;
    s.outer = 400.0f;
    uint8_t b[ugr::kBlobSize];
    ugr::encode(s, b);
    ugr::Style d;
    CHECK(b[1] == 0x0F && b[2] == 3 && b[18] == 64 && b[14] == 0 && b[15] == 0 && b[37] == 180);
    CHECK(ugr::decode(b, 40, 1, 61, &d));   // whatever the local settings, the blob is one a receiver takes
}

static void test_cap() {
    int side = 20, front = 12, rear = 12;   // Lv3 of the default shape: 64 lights
    ugr::cap_lights(&side, &front, &rear, ugr::kMaxLights);
    CHECK(2 * side + front + rear == 32);
    CHECK(side == 10 && front == 6 && rear == 6);
    side = 10, front = 6, rear = 6;   // Lv1: 32, as it is
    ugr::cap_lights(&side, &front, &rear, ugr::kMaxLights);
    CHECK(side == 10 && front == 6 && rear == 6);
    side = 0, front = 64, rear = 0;
    ugr::cap_lights(&side, &front, &rear, ugr::kMaxLights);
    CHECK(side == 0 && front == 32 && rear == 0);
    side = -3, front = 99, rear = 7;
    ugr::cap_lights(&side, &front, &rear, ugr::kMaxLights);
    CHECK(side == 0 && front + rear == 32 && rear <= 7);
}

static void test_select() {
    const float me[3] = {100.0f, 0.0f, 100.0f};
    ugr::Candidate c[12];
    for (int i = 0; i < 12; ++i) c[i] = {uintptr_t(0x1000 + i), {100.0f + 10.0f * float(12 - i), 0.0f, 100.0f}};
    // distances 120, 110, ..., 10: the 8 nearest are 11, 10, ..., 4
    int out[ugr::kMaxCars];
    int k = ugr::select_nearest(c, 12, me, ugr::kMaxDistance, ugr::kMaxCars, out);
    CHECK(k == 8);
    for (int j = 0; j < k; ++j) CHECK(out[j] == 11 - j);
    // past 150 m (3D) never counts
    c[11].pos[1] = 151.0f;
    c[10].pos[0] = 100.0f + 149.9f;
    c[10].pos[1] = 0.0f;
    k = ugr::select_nearest(c, 12, me, ugr::kMaxDistance, ugr::kMaxCars, out);
    CHECK(k == 8);
    for (int j = 0; j < k; ++j) CHECK(out[j] != 11);
    // a car exactly at 150 m counts; NaN never does
    ugr::Candidate e[3] = {{1, {250.0f, 0.0f, 100.0f}}, {2, {NAN, 0.0f, 0.0f}}, {3, {INFINITY, 0.0f, 0.0f}}};
    k = ugr::select_nearest(e, 3, me, ugr::kMaxDistance, ugr::kMaxCars, out);
    CHECK(k == 1 && out[0] == 0);
    // fewer than 8, unsorted input; equal distances: the lower key first
    ugr::Candidate f[4] = {{9, {100.0f, 0.0f, 150.0f}}, {5, {100.0f, 0.0f, 50.0f}}, {7, {120.0f, 0.0f, 100.0f}},
                           {4, {130.0f, 0.0f, 100.0f}}};
    k = ugr::select_nearest(f, 4, me, ugr::kMaxDistance, ugr::kMaxCars, out);
    CHECK(k == 4 && out[0] == 2 && out[1] == 3 && out[2] == 1 && out[3] == 0);
    CHECK(ugr::select_nearest(f, 0, me, ugr::kMaxDistance, ugr::kMaxCars, out) == 0);
    // only the first max_out kept, even when the nearest come last
    k = ugr::select_nearest(f, 4, me, ugr::kMaxDistance, 2, out);
    CHECK(k == 2 && out[0] == 2 && out[1] == 3);
}

int main() {
    test_golden();
    test_roundtrip();
    test_refusals();
    test_encode_clamps();
    test_cap();
    test_select();
    std::printf("ug_remote_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
