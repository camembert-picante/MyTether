// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for the message code and the USB device search. No phone or admin rights needed.
#include "../core/phone.c"
#include "../core/rndis.h"
#include <stdio.h>

static int failures;
#define CHECK(x)                                                \
    do {                                                        \
        if (!(x)) {                                             \
            printf("FAILED line %d: %s\n", __LINE__, #x);       \
            failures++;                                         \
        }                                                       \
    } while (0)

static uint32_t at(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
static void set(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

static void test_requests(void) {
    uint8_t m[64];
    CHECK(rndis_build_init(m, 7, 16384) == 24);
    CHECK(at(m) == 2 && at(m + 4) == 24 && at(m + 8) == 7 && at(m + 12) == 1 && at(m + 20) == 16384);
    CHECK(rndis_build_query(m, 8, RNDIS_OID_MAC) == 28);
    CHECK(at(m) == 4 && at(m + 12) == RNDIS_OID_MAC && at(m + 20) == 20);
    CHECK(rndis_build_set_u32(m, 9, RNDIS_OID_FILTER, RNDIS_FILTER_NORMAL) == 32);
    CHECK(at(m) == 5 && at(m + 16) == 4 && at(m + 20) == 20 && at(m + 28) == RNDIS_FILTER_NORMAL);
    CHECK(rndis_build_halt(m, 10) == 12);
    CHECK(at(m) == 3 && at(m + 4) == 12);
}

static void test_replies(void) {
    uint8_t m[64] = {0};
    rndis_caps caps;
    set(m, 0x80000002);
    set(m + 8, 1);
    set(m + 32, 15);
    set(m + 36, 16384);
    set(m + 40, 4);
    CHECK(rndis_is_reply(m, 52, 1));
    CHECK(!rndis_is_reply(m, 52, 2));
    CHECK(!rndis_is_reply(m, 10, 1));
    CHECK(rndis_read_init(m, 52, &caps));
    CHECK(caps.max_packets == 15 && caps.max_transfer == 16384 && caps.align == 16);
    CHECK(!rndis_read_init(m, 51, &caps));
    set(m + 40, 200);
    CHECK(rndis_read_init(m, 52, &caps) && caps.align == 1);
    set(m + 12, 1);
    CHECK(!rndis_read_init(m, 52, &caps));

    memset(m, 0, sizeof m);
    const uint8_t *data;
    size_t size;
    set(m, 0x80000004);
    set(m + 16, 6);
    set(m + 20, 16);
    memcpy(m + 24, "\1\2\3\4\5\6", 6);
    CHECK(rndis_read_query(m, 30, &data, &size) && size == 6 && data[5] == 6);
    CHECK(!rndis_read_query(m, 29, &data, &size));
    set(m + 16, 0xFFFFFFFF);
    CHECK(!rndis_read_query(m, 30, &data, &size));
    set(m + 16, 6);
    set(m + 20, 0xFFFFFFF0);
    CHECK(!rndis_read_query(m, 30, &data, &size));
}

typedef struct {
    size_t count, bytes;
    uint8_t first;
} seen;

static void note_frame(const uint8_t *frame, size_t len, void *ctx) {
    seen *s = ctx;
    if (!s->count) s->first = frame[0];
    s->count++;
    s->bytes += len;
}

static void test_batch_round_trip(void) {
    uint8_t out[2049], frame[100];
    memset(frame, 0xAB, sizeof frame);
    rndis_caps caps = {.max_packets = 3, .max_transfer = 2048, .align = 16};
    rndis_batch b;
    rndis_batch_init(&b, out, 2048, &caps, 512);
    CHECK(rndis_batch_add(&b, frame, 100));
    CHECK(rndis_batch_add(&b, frame, 50));
    CHECK(rndis_batch_add(&b, frame, 1));
    CHECK(!rndis_batch_add(&b, frame, 1));  // the phone's limit of 3 frames
    CHECK(at(out + 4) == 144);              // 144 is already on a 16-byte boundary
    CHECK(at(out + 144 + 4) == 96);         // 94 rounded up to 96
    size_t n = rndis_batch_finish(&b);
    CHECK(n == 144 + 96 + 45);
    seen s = {0};
    CHECK(rndis_unpack(out, n, note_frame, &s) == 3);
    CHECK(s.count == 3 && s.bytes == 151 && s.first == 0xAB);
    CHECK(rndis_batch_add(&b, frame, 1));  // finish emptied it
}

static void test_batch_limits(void) {
    uint8_t out[1025], frame[1024] = {0};
    rndis_caps caps = {.max_packets = 10, .max_transfer = 1024, .align = 1};
    rndis_batch b;
    rndis_batch_init(&b, out, 1024, &caps, 512);
    CHECK(!rndis_batch_add(&b, frame, 1024 - RNDIS_PACKET_HEADER + 1));

    // A transfer of exactly one USB packet gets one extra byte, and it still parses.
    CHECK(rndis_batch_add(&b, frame, 512 - RNDIS_PACKET_HEADER));
    size_t n = rndis_batch_finish(&b);
    CHECK(n == 513 && at(out + 4) == 513);
    seen s = {0};
    CHECK(rndis_unpack(out, n, note_frame, &s) == 1 && s.bytes == 512 - RNDIS_PACKET_HEADER);

    // Filling the whole buffer still leaves room for that byte.
    CHECK(rndis_batch_add(&b, frame, 1024 - RNDIS_PACKET_HEADER));
    CHECK(rndis_batch_finish(&b) == 1025);
    CHECK(rndis_batch_finish(&b) == 0);
}

static void test_unpack_rejects_bad_input(void) {
    uint8_t m[128] = {0};
    seen s = {0};
    CHECK(rndis_unpack(m, 0, note_frame, &s) == 0);
    CHECK(rndis_unpack(m, 43, note_frame, &s) == 0);
    CHECK(rndis_unpack(m, 128, note_frame, &s) == 0);  // wrong message type
    set(m, 1);
    set(m + 4, 0);
    CHECK(rndis_unpack(m, 128, note_frame, &s) == 0);  // a zero length would never advance
    set(m + 4, 129);
    CHECK(rndis_unpack(m, 128, note_frame, &s) == 0);  // longer than the transfer
    set(m + 4, 64);
    set(m + 8, 36);
    set(m + 12, 21);
    CHECK(rndis_unpack(m, 128, note_frame, &s) == 0);  // frame runs past its message
    set(m + 12, 0xFFFFFFFF);
    CHECK(rndis_unpack(m, 128, note_frame, &s) == 0);
    set(m + 8, 0xFFFFFFF0);
    set(m + 12, 16);
    CHECK(rndis_unpack(m, 128, note_frame, &s) == 0);  // offset wraps around
    set(m + 8, 0);
    CHECK(rndis_unpack(m, 128, note_frame, &s) == 0);  // frame overlaps the header
    CHECK(s.count == 0);

    // A good message followed by junk gives the good frame and stops.
    set(m + 8, 36);
    set(m + 12, 20);
    CHECK(rndis_unpack(m, 128, note_frame, &s) == 1 && s.bytes == 20);
}

static void test_garbage_never_crashes(void) {
    uint8_t m[512];
    seen s = {0};
    uint32_t x = 2463534242u;
    for (int round = 0; round < 200000; round++) {
        for (size_t i = 0; i < sizeof m; i += 4) {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            // Mostly small values, so plenty of messages get past the first checks.
            set(m + i, (i % 8 == 0 ? 1 : x & 0xFF));
        }
        rndis_unpack(m, x % sizeof m, note_frame, &s);
    }
    CHECK(true);
}

static bool any_interface(int cls, int sub, int proto) {
    (void)sub;
    (void)proto;
    return cls >= 0;
}

static void test_usb_search(void) {
    // Whatever is plugged in, the search must not confuse it with a phone unless it is one.
    CHECK(is_tether(0xE0, 1, 3) && is_tether(0xEF, 4, 1) && is_tether(2, 2, 0xFF));
    CHECK(!is_tether(3, 1, 2) && !is_tether(-1, -1, -1) && !is_tether(8, 6, 80));
    char name[128] = "";
    if (find_interface(any_interface, name, sizeof name, NULL, NULL)) {
        printf("USB search sees: %s\n", name);
        CHECK(name[0] != 0);
    }
}

int main(void) {
    test_requests();
    test_replies();
    test_batch_round_trip();
    test_batch_limits();
    test_unpack_rejects_bad_input();
    test_garbage_never_crashes();
    test_usb_search();
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures != 0;
}
