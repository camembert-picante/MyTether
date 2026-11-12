// SPDX-License-Identifier: GPL-3.0-or-later
#include "usb.h"
#include "phone.h"
#include <string.h>

static const char *const NO_PHONE = "No phone found. Turn on USB tethering on the phone.";
static const char *const BUSY = "Another app is using the phone. Close it and try again.";
static const char *const NO_ACCESS = "Couldn't talk to the phone. Unplug it and try again.";

static IOUSBInterfaceInterface650 **open_interface(io_service_t s, IOReturn *result) {
    IOCFPlugInInterface **plugin = NULL;
    IOUSBInterfaceInterface650 **i = NULL;
    SInt32 score;
    *result = IOCreatePlugInInterfaceForService(s, kIOUSBInterfaceUserClientTypeID,
                                                kIOCFPlugInInterfaceID, &plugin, &score);
    if (*result != kIOReturnSuccess || !plugin) return NULL;
    (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID650),
                              (LPVOID *)&i);
    IODestroyPlugInInterface(plugin);
    if (!i) {
        *result = kIOReturnUnsupported;
        return NULL;
    }
    *result = (*i)->USBInterfaceOpen(i);
    if (*result != kIOReturnSuccess) {
        (*i)->Release(i);
        return NULL;
    }
    return i;
}

static bool find_bulk_pipes(usb_link *l) {
    UInt8 count = 0;
    (*l->data)->GetNumEndpoints(l->data, &count);
    for (UInt8 pipe = 1; pipe <= count; pipe++) {
        UInt8 direction, number, type, interval;
        UInt16 packet;
        if ((*l->data)->GetPipeProperties(l->data, pipe, &direction, &number, &type, &packet,
                                          &interval) != kIOReturnSuccess || type != kUSBBulk)
            continue;
        if (direction == kUSBIn) {
            l->in = pipe;
        } else {
            l->out = pipe;
            l->out_packet = packet;
        }
    }
    return l->in && l->out;
}

const char *usb_open(usb_link *l, char *name, size_t cap) {
    memset(l, 0, sizeof *l);
    io_service_t comm = 0, data = 0;
    if (!phone_find(name, cap, &comm, &data)) return NO_PHONE;

    const char *error = NULL;
    IOReturn r = kIOReturnError;
    if (!data) {
        error = NO_ACCESS;
    } else if (!(l->comm = open_interface(comm, &r)) || !(l->data = open_interface(data, &r))) {
        error = r == kIOReturnExclusiveAccess ? BUSY : NO_ACCESS;
    } else if ((*l->comm)->GetInterfaceNumber(l->comm, &l->comm_number) != kIOReturnSuccess ||
               !find_bulk_pipes(l)) {
        error = NO_ACCESS;
    } else {
        // An earlier session that ended abruptly can leave the pipes out of step.
        usb_clear_in(l);
        usb_clear_out(l);
    }
    IOObjectRelease(comm);
    if (data) IOObjectRelease(data);
    if (error) usb_close(l);
    return error;
}

void usb_close(usb_link *l) {
    if (l->data) {
        (*l->data)->USBInterfaceClose(l->data);
        (*l->data)->Release(l->data);
    }
    if (l->comm) {
        (*l->comm)->USBInterfaceClose(l->comm);
        (*l->comm)->Release(l->comm);
    }
    memset(l, 0, sizeof *l);
}

static IOUSBDevRequestTO control(usb_link *l, UInt8 type, UInt8 request, void *buf, size_t len) {
    IOUSBDevRequestTO r = {0};
    r.bmRequestType = type;
    r.bRequest = request;
    r.wIndex = l->comm_number;
    r.wLength = (UInt16)len;
    r.pData = buf;
    r.noDataTimeout = 1000;
    r.completionTimeout = 2000;
    return r;
}

IOReturn usb_command(usb_link *l, void *buf, size_t len) {
    IOUSBDevRequestTO r = control(l, USBmakebmRequestType(kUSBOut, kUSBClass, kUSBInterface),
                                  0, buf, len);
    return (*l->comm)->ControlRequestTO(l->comm, 0, &r);
}

IOReturn usb_response(usb_link *l, void *buf, size_t cap, size_t *got) {
    IOUSBDevRequestTO r = control(l, USBmakebmRequestType(kUSBIn, kUSBClass, kUSBInterface),
                                  1, buf, cap);
    IOReturn result = (*l->comm)->ControlRequestTO(l->comm, 0, &r);
    *got = r.wLenDone;
    return result;
}

IOReturn usb_attach(usb_link *l, CFRunLoopRef loop) {
    CFRunLoopSourceRef source = NULL;
    IOReturn r = (*l->data)->CreateInterfaceAsyncEventSource(l->data, &source);
    if (r == kIOReturnSuccess) CFRunLoopAddSource(loop, source, kCFRunLoopDefaultMode);
    return r;
}

IOReturn usb_read(usb_link *l, void *buf, uint32_t size, IOAsyncCallback1 done, void *ctx) {
    return (*l->data)->ReadPipeAsync(l->data, l->in, buf, size, done, ctx);
}

IOReturn usb_write(usb_link *l, void *buf, uint32_t size, IOAsyncCallback1 done, void *ctx) {
    return (*l->data)->WritePipeAsync(l->data, l->out, buf, size, done, ctx);
}

void usb_abort(usb_link *l) {
    (*l->data)->AbortPipe(l->data, l->in);
    (*l->data)->AbortPipe(l->data, l->out);
}

void usb_abort_write(usb_link *l) { (*l->data)->AbortPipe(l->data, l->out); }
void usb_clear_in(usb_link *l) { (*l->data)->ClearPipeStallBothEnds(l->data, l->in); }
void usb_clear_out(usb_link *l) { (*l->data)->ClearPipeStallBothEnds(l->data, l->out); }
