// SPDX-License-Identifier: GPL-3.0-or-later
// A claimed phone: control requests plus the two bulk pipes that carry network traffic.
#pragma once
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <stdbool.h>

typedef struct {
    IOUSBInterfaceInterface650 **comm, **data;
    uint8_t comm_number, in, out;
    uint16_t out_packet;
} usb_link;

// Returns NULL on success, otherwise a sentence to show the user.
const char *usb_open(usb_link *l, char *name, size_t cap);
void usb_close(usb_link *l);

IOReturn usb_command(usb_link *l, void *buf, size_t len);
IOReturn usb_response(usb_link *l, void *buf, size_t cap, size_t *got);

// Transfers finish on the run loop passed to usb_attach.
IOReturn usb_attach(usb_link *l, CFRunLoopRef loop);
IOReturn usb_read(usb_link *l, void *buf, uint32_t size, IOAsyncCallback1 done, void *ctx);
IOReturn usb_write(usb_link *l, void *buf, uint32_t size, IOAsyncCallback1 done, void *ctx);
void usb_abort(usb_link *l);
void usb_abort_write(usb_link *l);
void usb_clear_in(usb_link *l);
void usb_clear_out(usb_link *l);
