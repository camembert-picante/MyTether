// SPDX-License-Identifier: GPL-3.0-or-later
#include "rndis.h"
#include <string.h>

enum {
    MSG_PACKET = 1,
    MSG_INIT = 2,
    MSG_HALT = 3,
    MSG_QUERY = 4,
    MSG_SET = 5,
    REPLY_FLAG = 0x80000000u,
};

// RNDIS is little-endian, and so is every Mac this builds for.
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static uint32_t get32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static size_t header(uint8_t *out, uint32_t type, uint32_t len, uint32_t id) {
    memset(out, 0, len);
    put32(out, type);
    put32(out + 4, len);
    put32(out + 8, id);
    return len;
}

size_t rndis_build_init(uint8_t *out, uint32_t id, uint32_t host_max_transfer) {
    size_t n = header(out, MSG_INIT, 24, id);
    put32(out + 12, 1);  // protocol version 1.0
    put32(out + 20, host_max_transfer);
    return n;
}

size_t rndis_build_query(uint8_t *out, uint32_t id, uint32_t oid) {
    size_t n = header(out, MSG_QUERY, 28, id);
    put32(out + 12, oid);
    put32(out + 20, 20);  // offsets count from byte 8
    return n;
}

size_t rndis_build_set_u32(uint8_t *out, uint32_t id, uint32_t oid, uint32_t value) {
    size_t n = header(out, MSG_SET, 32, id);
    put32(out + 12, oid);
    put32(out + 16, 4);
    put32(out + 20, 20);
    put32(out + 28, value);
    return n;
}

size_t rndis_build_halt(uint8_t *out, uint32_t id) { return header(out, MSG_HALT, 12, id); }

bool rndis_is_reply(const uint8_t *msg, size_t len, uint32_t id) {
    return len >= 16 && (get32(msg) & REPLY_FLAG) && get32(msg + 8) == id;
}

static bool reply_ok(const uint8_t *msg, size_t len, uint32_t type, size_t min) {
    return len >= min && get32(msg) == (type | REPLY_FLAG) && get32(msg + 12) == 0;
}

bool rndis_read_init(const uint8_t *msg, size_t len, rndis_caps *caps) {
    if (!reply_ok(msg, len, MSG_INIT, 52)) return false;
    caps->max_packets = get32(msg + 32);
    caps->max_transfer = get32(msg + 36);
    uint32_t exponent = get32(msg + 40);
    caps->align = exponent > 7 ? 1 : 1u << exponent;
    if (caps->max_packets == 0) caps->max_packets = 1;
    return true;
}

bool rndis_read_query(const uint8_t *msg, size_t len, const uint8_t **data, size_t *data_len) {
    if (!reply_ok(msg, len, MSG_QUERY, 24)) return false;
    uint64_t size = get32(msg + 16), start = 8 + (uint64_t)get32(msg + 20);
    if (start + size > len) return false;
    *data = msg + start;
    *data_len = (size_t)size;
    return true;
}

bool rndis_read_set(const uint8_t *msg, size_t len) { return reply_ok(msg, len, MSG_SET, 16); }

size_t rndis_unpack(const uint8_t *buf, size_t len, rndis_frame_fn fn, void *ctx) {
    size_t at = 0, frames = 0;
    while (len - at >= RNDIS_PACKET_HEADER) {
        const uint8_t *m = buf + at;
        uint32_t msg_len = get32(m + 4);
        if (get32(m) != MSG_PACKET || msg_len < RNDIS_PACKET_HEADER || msg_len > len - at) break;
        uint64_t start = 8 + (uint64_t)get32(m + 8), size = get32(m + 12);
        if (start < RNDIS_PACKET_HEADER || start + size > msg_len) break;
        if (size) {
            fn(m + start, (size_t)size, ctx);
            frames++;
        }
        at += msg_len;
    }
    return frames;
}

void rndis_batch_init(rndis_batch *b, uint8_t *buf, size_t cap, const rndis_caps *caps,
                      uint32_t usb_packet) {
    memset(b, 0, sizeof *b);
    b->buf = buf;
    b->cap = cap;
    b->max_packets = caps->max_packets;
    b->align = caps->align ? caps->align : 1;
    b->usb_packet = usb_packet ? usb_packet : 512;
}

bool rndis_batch_add(rndis_batch *b, const uint8_t *frame, size_t len) {
    size_t start = b->used;
    if (b->count) start = (start + b->align - 1) / b->align * b->align;
    if (b->count >= b->max_packets || start + RNDIS_PACKET_HEADER + len > b->cap) return false;
    if (start > b->used) {
        // The padding counts as part of the message before it.
        memset(b->buf + b->used, 0, start - b->used);
        put32(b->buf + b->last + 4, get32(b->buf + b->last + 4) + (uint32_t)(start - b->used));
    }
    uint8_t *m = b->buf + start;
    memset(m, 0, RNDIS_PACKET_HEADER);
    put32(m, MSG_PACKET);
    put32(m + 4, RNDIS_PACKET_HEADER + (uint32_t)len);
    put32(m + 8, RNDIS_PACKET_HEADER - 8);
    put32(m + 12, (uint32_t)len);
    memcpy(m + RNDIS_PACKET_HEADER, frame, len);
    b->last = start;
    b->used = start + RNDIS_PACKET_HEADER + len;
    b->count++;
    return true;
}

size_t rndis_batch_finish(rndis_batch *b) {
    // A transfer that ends exactly on a USB packet boundary looks unfinished to the phone.
    // One extra byte of padding ends it. This is why buf holds cap + 1.
    if (b->used && b->used % b->usb_packet == 0) {
        b->buf[b->used++] = 0;
        put32(b->buf + b->last + 4, get32(b->buf + b->last + 4) + 1);
    }
    size_t n = b->used;
    b->used = b->last = 0;
    b->count = 0;
    return n;
}
