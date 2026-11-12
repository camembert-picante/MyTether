// SPDX-License-Identifier: GPL-3.0-or-later
// The Mac's side of the connection: a virtual Ethernet pair and a network service.
// Everything here needs root.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// macOS sees HOST_IF as an ordinary network port. We read and write packets on its other
// end, DRIVER_IF. The numbers are high so they stay clear of other apps that use feth.
#define HOST_IF "feth170"
#define DRIVER_IF "feth171"

bool net_create(const uint8_t mac[6]);
void net_destroy(void);

// Returns an open descriptor on DRIVER_IF, or -1. buf_len is the size every read must use.
int net_open_packets(uint32_t *buf_len);

// Adds the service that makes macOS ask the phone for an address. first puts it above
// Wi-Fi in the service order, so traffic prefers the phone.
bool net_add_service(bool first);
void net_set_first(bool first);
void net_remove_services(void);

// Writes HOST_IF's address to out and returns true once it has a real one.
bool net_address(char *out, size_t cap);
