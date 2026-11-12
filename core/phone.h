// SPDX-License-Identifier: GPL-3.0-or-later
// Finds a tethering phone in the list of USB devices. Needs no special rights, so the app
// and the helper both use it.
#pragma once
#include <IOKit/IOKitLib.h>
#include <stdbool.h>
#include <stddef.h>

// Fills name with the phone's name. comm and data may be NULL. When set, the caller
// releases them with IOObjectRelease.
bool phone_find(char *name, size_t cap, io_service_t *comm, io_service_t *data);

// Calls changed on the main queue whenever any USB device comes or goes. It waits for
// the system to call it, so an idle Mac does no work here.
void phone_watch(void (*changed)(void *ctx), void *ctx);
