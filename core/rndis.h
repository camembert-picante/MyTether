// SPDX-License-Identifier: GPL-3.0-or-later
// RNDIS is how an Android phone offers USB tethering. This file builds and reads its
// messages and does no I/O, so the tests can run without a phone.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RNDIS_OID_MAC 0x01010101u
#define RNDIS_OID_FILTER 0x0001010Eu
// Receive frames sent to us, to multicast groups, and to everyone.
#define RNDIS_FILTER_NORMAL 0x0000000Fu
#define RNDIS_PACKET_HEADER 44u

typedef struct {
    uint32_t max_packets;   // frames the phone accepts in one USB transfer
    uint32_t max_transfer;  // bytes the phone accepts in one USB transfer
    uint32_t align;         // each message in a transfer starts on this boundary
} rndis_caps;

size_t rndis_build_init(uint8_t *out, uint32_t id, uint32_t host_max_transfer);
size_t rndis_build_query(uint8_t *out, uint32_t id, uint32_t oid);
size_t rndis_build_set_u32(uint8_t *out, uint32_t id, uint32_t oid, uint32_t value);
size_t rndis_build_halt(uint8_t *out, uint32_t id);

// True when msg answers the request with this id. The phone can queue unrelated status
// messages, so replies are matched by id and everything else is ignored.
bool rndis_is_reply(const uint8_t *msg, size_t len, uint32_t id);
bool rndis_read_init(const uint8_t *msg, size_t len, rndis_caps *caps);
bool rndis_read_query(const uint8_t *msg, size_t len, const uint8_t **data, size_t *data_len);
bool rndis_read_set(const uint8_t *msg, size_t len);

// Calls fn once per Ethernet frame in a received transfer and returns the frame count.
// The phone is untrusted input: anything malformed ends the walk.
typedef void (*rndis_frame_fn)(const uint8_t *frame, size_t len, void *ctx);
size_t rndis_unpack(const uint8_t *buf, size_t len, rndis_frame_fn fn, void *ctx);

// Packs outgoing frames into one USB transfer. buf must hold cap + 1 bytes.
typedef struct {
    uint8_t *buf;
    size_t cap, used, last;
    uint32_t count, max_packets, align, usb_packet;
} rndis_batch;

void rndis_batch_init(rndis_batch *b, uint8_t *buf, size_t cap, const rndis_caps *caps,
                      uint32_t usb_packet);
// False when the frame doesn't fit: send what's there and add it again.
bool rndis_batch_add(rndis_batch *b, const uint8_t *frame, size_t len);
// Returns the bytes to send and empties the batch.
size_t rndis_batch_finish(rndis_batch *b);
