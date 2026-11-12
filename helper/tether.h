// SPDX-License-Identifier: GPL-3.0-or-later
// One tethering session: the phone, the virtual network port, and the traffic between them.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Returns NULL on success, otherwise a sentence to show the user. ended runs on a
// background thread if the phone goes away by itself.
const char *tether_start(bool prefer_phone, void (*ended)(void));
void tether_stop(void);

const char *tether_phone(void);
uint64_t tether_rx_bytes(void);
uint64_t tether_tx_bytes(void);

// Runs everything that doesn't need root against a real phone, then prints what happened.
// Returns a process exit code.
int tether_probe(int seconds);
