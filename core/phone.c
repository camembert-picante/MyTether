// SPDX-License-Identifier: GPL-3.0-or-later
#include "phone.h"
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <stdlib.h>
#include <string.h>

static int number(io_service_t s, CFStringRef key) {
    CFTypeRef v = IORegistryEntryCreateCFProperty(s, key, kCFAllocatorDefault, 0);
    int out = -1;
    if (v && CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue(v, kCFNumberIntType, &out);
    if (v) CFRelease(v);
    return out;
}

static bool text(io_service_t s, CFStringRef key, char *out, size_t cap) {
    CFTypeRef v = IORegistryEntryCreateCFProperty(s, key, kCFAllocatorDefault, 0);
    bool ok = v && CFGetTypeID(v) == CFStringGetTypeID() &&
              CFStringGetCString(v, out, (CFIndex)cap, kCFStringEncodingUTF8);
    if (v) CFRelease(v);
    return ok;
}

// Phones announce tethering in one of these three ways.
static bool is_tether(int cls, int sub, int proto) {
    return (cls == 0xE0 && sub == 0x01 && proto == 0x03) ||
           (cls == 0xEF && sub == 0x04 && proto == 0x01) ||
           (cls == 0x02 && sub == 0x02 && proto == 0xFF);
}

static bool interface_matches(io_service_t s, bool (*match)(int, int, int)) {
    return match(number(s, CFSTR("bInterfaceClass")), number(s, CFSTR("bInterfaceSubClass")),
                 number(s, CFSTR("bInterfaceProtocol")));
}

// The data interface is a sibling of the control interface, normally the next one.
static io_service_t data_interface(io_service_t device, int comm_number) {
    io_iterator_t it;
    io_service_t best = 0, child;
    if (IORegistryEntryGetChildIterator(device, kIOServicePlane, &it) != KERN_SUCCESS) return 0;
    while ((child = IOIteratorNext(it))) {
        bool next = number(child, CFSTR("bInterfaceNumber")) == comm_number + 1;
        if (number(child, CFSTR("bInterfaceClass")) == 0x0A && (next || !best)) {
            if (best) IOObjectRelease(best);
            best = child;
            if (next) break;
        } else {
            IOObjectRelease(child);
        }
    }
    IOObjectRelease(it);
    return best;
}

static bool find_interface(bool (*match)(int, int, int), char *name, size_t cap, io_service_t *comm,
                           io_service_t *data) {
    io_iterator_t it;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOUSBHostInterface"),
                                     &it) != KERN_SUCCESS)
        return false;
    bool found = false;
    io_service_t s;
    while (!found && (s = IOIteratorNext(it))) {
        if (interface_matches(s, match)) {
            found = true;
            if (name && !text(s, CFSTR("USB Product Name"), name, cap))
                strlcpy(name, "Android phone", cap);
            if (data) {
                io_registry_entry_t device = 0;
                *data = 0;
                if (IORegistryEntryGetParentEntry(s, kIOServicePlane, &device) == KERN_SUCCESS) {
                    *data = data_interface(device, number(s, CFSTR("bInterfaceNumber")));
                    IOObjectRelease(device);
                }
            }
            if (comm) {
                *comm = s;
                continue;
            }
        }
        IOObjectRelease(s);
    }
    IOObjectRelease(it);
    return found;
}

bool phone_find(char *name, size_t cap, io_service_t *comm, io_service_t *data) {
    return find_interface(is_tether, name, cap, comm, data);
}

typedef struct {
    void (*changed)(void *);
    void *ctx;
} watcher;

static void on_usb_event(void *refcon, io_iterator_t it) {
    // The system only notifies again once the iterator has been read to the end.
    io_service_t s;
    while ((s = IOIteratorNext(it))) IOObjectRelease(s);
    watcher *w = refcon;
    w->changed(w->ctx);
}

void phone_watch(void (*changed)(void *ctx), void *ctx) {
    watcher *w = malloc(sizeof *w);
    w->changed = changed;
    w->ctx = ctx;
    IONotificationPortRef port = IONotificationPortCreate(kIOMainPortDefault);
    IONotificationPortSetDispatchQueue(port, dispatch_get_main_queue());
    const io_name_t events[2] = {kIOFirstMatchNotification, kIOTerminatedNotification};
    for (int i = 0; i < 2; i++) {
        io_iterator_t it;
        if (IOServiceAddMatchingNotification(port, events[i], IOServiceMatching("IOUSBHostInterface"),
                                             on_usb_event, w, &it) == KERN_SUCCESS) {
            io_service_t s;
            while ((s = IOIteratorNext(it))) IOObjectRelease(s);
        }
    }
}
