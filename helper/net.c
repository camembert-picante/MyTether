// SPDX-License-Identifier: GPL-3.0-or-later
#include "net.h"
#include <SystemConfiguration/SystemConfiguration.h>
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/bpf.h>
#include <net/if.h>
#include <netinet/in.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define SERVICE_NAME CFSTR("MyTether")
#define PACKET_BUFFER (1024 * 1024)

static bool ifconfig(const char *a, const char *b, const char *c, const char *d) {
    char *argv[] = {"ifconfig", (char *)a, (char *)b, (char *)c, (char *)d, NULL};
    pid_t pid;
    int status = -1;
    if (posix_spawn(&pid, "/sbin/ifconfig", NULL, NULL, argv, environ) != 0) return false;
    while (waitpid(pid, &status, 0) < 0) {}
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

void net_destroy(void) {
    if (if_nametoindex(HOST_IF)) ifconfig(HOST_IF, "destroy", NULL, NULL);
    if (if_nametoindex(DRIVER_IF)) ifconfig(DRIVER_IF, "destroy", NULL, NULL);
}

bool net_create(const uint8_t mac[6]) {
    // The phone only answers traffic sent from the address it handed us, and the address
    // has to be set before the port comes up.
    char address[18];
    snprintf(address, sizeof address, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2],
             mac[3], mac[4], mac[5]);
    net_destroy();
    if (ifconfig(HOST_IF, "create", NULL, NULL) && ifconfig(DRIVER_IF, "create", NULL, NULL) &&
        ifconfig(HOST_IF, "peer", DRIVER_IF, NULL) && ifconfig(HOST_IF, "lladdr", address, NULL) &&
        ifconfig(HOST_IF, "mtu", "1500", "up") && ifconfig(DRIVER_IF, "mtu", "1500", "up"))
        return true;
    net_destroy();
    return false;
}

int net_open_packets(uint32_t *buf_len) {
    int fd = -1;
    for (int i = 0; i < 256 && fd < 0; i++) {
        char path[16];
        snprintf(path, sizeof path, "/dev/bpf%d", i);
        fd = open(path, O_RDWR);
        if (fd < 0 && errno != EBUSY) break;
    }
    if (fd < 0) return -1;

    u_int size = PACKET_BUFFER, on = 1, off = 0;
    struct ifreq ifr = {0};
    strlcpy(ifr.ifr_name, DRIVER_IF, sizeof ifr.ifr_name);
    // The one-second read timeout lets the sending thread notice a stop request.
    struct timeval timeout = {.tv_sec = 1};
    // BIOCSHDRCMPLT: our frames already carry their full Ethernet header.
    // BIOCSSEESENT off: without it we would read back our own frames and loop forever.
    if (ioctl(fd, BIOCSBLEN, &size) < 0 || ioctl(fd, BIOCIMMEDIATE, &on) < 0 ||
        ioctl(fd, BIOCSHDRCMPLT, &on) < 0 || ioctl(fd, BIOCSETIF, &ifr) < 0 ||
        ioctl(fd, BIOCSSEESENT, &off) < 0 || ioctl(fd, BIOCSRTIMEOUT, &timeout) < 0 ||
        ioctl(fd, BIOCGBLEN, &size) < 0) {
        close(fd);
        return -1;
    }
    *buf_len = size;
    return fd;
}

// Apple's public calls only list real hardware ports. This one also accepts a virtual
// port. It's private, so it is looked up by name and its absence is handled.
typedef SCNetworkInterfaceRef (*create_interface_fn)(CFAllocatorRef, CFStringRef, UInt32);
#define ALL_VIRTUAL_INTERFACES 0xFFFFFFFFu

static SCPreferencesRef open_prefs(void) {
    SCPreferencesRef prefs = SCPreferencesCreate(NULL, CFSTR("local.mytether.helperd"), NULL);
    if (prefs && !SCPreferencesLock(prefs, true)) {
        CFRelease(prefs);
        return NULL;
    }
    return prefs;
}

static void close_prefs(SCPreferencesRef prefs, bool save) {
    if (save && SCPreferencesCommitChanges(prefs)) SCPreferencesApplyChanges(prefs);
    SCPreferencesUnlock(prefs);
    CFRelease(prefs);
}

static SCNetworkServiceRef find_service(SCPreferencesRef prefs) {
    CFArrayRef all = SCNetworkServiceCopyAll(prefs);
    SCNetworkServiceRef found = NULL;
    for (CFIndex i = 0; all && i < CFArrayGetCount(all) && !found; i++) {
        SCNetworkServiceRef s = CFArrayGetValueAtIndex(all, i);
        CFStringRef name = SCNetworkServiceGetName(s);
        if (name && CFStringHasPrefix(name, SERVICE_NAME)) found = (SCNetworkServiceRef)CFRetain(s);
    }
    if (all) CFRelease(all);
    return found;
}

static void put_in_order(SCNetworkSetRef set, SCNetworkServiceRef service, bool first) {
    CFStringRef id = SCNetworkServiceGetServiceID(service);
    CFArrayRef current = SCNetworkSetGetServiceOrder(set);
    CFMutableArrayRef order = current ? CFArrayCreateMutableCopy(NULL, 0, current)
                                      : CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    CFIndex at = CFArrayGetFirstIndexOfValue(order, CFRangeMake(0, CFArrayGetCount(order)), id);
    if (at != kCFNotFound) CFArrayRemoveValueAtIndex(order, at);
    if (first) CFArrayInsertValueAtIndex(order, 0, id);
    else CFArrayAppendValue(order, id);
    SCNetworkSetSetServiceOrder(set, order);
    CFRelease(order);
}

static void use_dhcp(SCNetworkServiceRef service) {
    const void *keys[] = {kSCPropNetIPv4ConfigMethod};
    const void *values[] = {kSCValNetIPv4ConfigMethodDHCP};
    CFDictionaryRef config = CFDictionaryCreate(NULL, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
                                                &kCFTypeDictionaryValueCallBacks);
    SCNetworkProtocolRef ipv4 = SCNetworkServiceCopyProtocol(service, kSCNetworkProtocolTypeIPv4);
    if (ipv4) {
        SCNetworkProtocolSetConfiguration(ipv4, config);
        CFRelease(ipv4);
    }
    CFRelease(config);
}

bool net_add_service(bool first) {
    create_interface_fn create_interface =
        (create_interface_fn)dlsym(RTLD_DEFAULT, "_SCNetworkInterfaceCreateWithBSDName");
    if (!create_interface) return false;

    SCPreferencesRef prefs = open_prefs();
    if (!prefs) return false;
    SCNetworkInterfaceRef port = create_interface(NULL, CFSTR(HOST_IF), ALL_VIRTUAL_INTERFACES);
    SCNetworkServiceRef service = port ? SCNetworkServiceCreate(prefs, port) : NULL;
    SCNetworkSetRef set = SCNetworkSetCopyCurrent(prefs);
    bool ok = service && set && SCNetworkServiceSetName(service, SERVICE_NAME) &&
              SCNetworkServiceEstablishDefaultConfiguration(service) &&
              SCNetworkSetAddService(set, service);
    if (ok) {
        use_dhcp(service);
        put_in_order(set, service, first);
    }
    close_prefs(prefs, ok);
    if (set) CFRelease(set);
    if (service) CFRelease(service);
    if (port) CFRelease(port);
    return ok;
}

void net_set_first(bool first) {
    SCPreferencesRef prefs = open_prefs();
    if (!prefs) return;
    SCNetworkServiceRef service = find_service(prefs);
    SCNetworkSetRef set = SCNetworkSetCopyCurrent(prefs);
    if (service && set) put_in_order(set, service, first);
    close_prefs(prefs, service && set);
    if (set) CFRelease(set);
    if (service) CFRelease(service);
}

void net_remove_services(void) {
    SCPreferencesRef prefs = open_prefs();
    if (!prefs) return;
    bool changed = false;
    SCNetworkServiceRef service;
    // Only services named MyTether are touched, including any a crash left behind.
    while ((service = find_service(prefs))) {
        bool removed = SCNetworkServiceRemove(service);
        CFRelease(service);
        if (!removed) break;
        changed = true;
    }
    close_prefs(prefs, changed);
}

bool net_address(char *out, size_t cap) {
    struct ifaddrs *list = NULL;
    if (getifaddrs(&list) != 0) return false;
    bool found = false;
    for (struct ifaddrs *a = list; a && !found; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET || strcmp(a->ifa_name, HOST_IF)) continue;
        struct in_addr ip = ((struct sockaddr_in *)(void *)a->ifa_addr)->sin_addr;
        // 169.254.x.x is what macOS picks for itself when nothing answered.
        if ((ntohl(ip.s_addr) >> 16) == 0xA9FE) continue;
        found = inet_ntop(AF_INET, &ip, out, (socklen_t)cap) != NULL;
    }
    freeifaddrs(list);
    return found;
}
