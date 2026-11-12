// SPDX-License-Identifier: GPL-3.0-or-later
// Checks the rule that lets the helper skip password prompts: only an app with the exact
// fingerprint saved at Set Up gets an answer.
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <dispatch/dispatch.h>
#include <stdio.h>
#include <string.h>
#include <xpc/xpc.h>

static bool own_fingerprint(char out[41]) {
    SecCodeRef self = NULL;
    SecStaticCodeRef file = NULL;
    CFDictionaryRef info = NULL;
    bool ok = false;
    if (SecCodeCopySelf(kSecCSDefaultFlags, &self) == errSecSuccess &&
        SecCodeCopyStaticCode(self, kSecCSDefaultFlags, &file) == errSecSuccess &&
        SecCodeCopySigningInformation(file, kSecCSDefaultFlags, &info) == errSecSuccess) {
        CFDataRef hash = CFDictionaryGetValue(info, kSecCodeInfoUnique);
        if (hash && CFDataGetLength(hash) == 20) {
            for (int i = 0; i < 20; i++) sprintf(out + i * 2, "%02x", CFDataGetBytePtr(hash)[i]);
            ok = true;
        }
    }
    if (info) CFRelease(info);
    if (file) CFRelease(file);
    if (self) CFRelease(self);
    return ok;
}

// Starts a listener that requires this fingerprint and reports whether a request got a reply.
static bool answered_with(const char *fingerprint) {
    dispatch_queue_t q = dispatch_queue_create("pin", NULL);
    xpc_connection_t listener = xpc_connection_create(NULL, q);
    char requirement[96];
    snprintf(requirement, sizeof requirement, "cdhash H\"%s\"", fingerprint);
    if (xpc_connection_set_peer_code_signing_requirement(listener, requirement) != 0) return false;
    xpc_connection_set_event_handler(listener, ^(xpc_object_t peer) {
        if (xpc_get_type(peer) != XPC_TYPE_CONNECTION) return;
        xpc_connection_set_event_handler(peer, ^(xpc_object_t m) {
            if (xpc_get_type(m) != XPC_TYPE_DICTIONARY) return;
            xpc_object_t r = xpc_dictionary_create_reply(m);
            xpc_dictionary_set_bool(r, "ok", true);
            xpc_connection_send_message(peer, r);
        });
        xpc_connection_activate(peer);
    });
    xpc_connection_activate(listener);

    xpc_connection_t c = xpc_connection_create_from_endpoint(xpc_endpoint_create(listener));
    xpc_connection_set_event_handler(c, ^(xpc_object_t e) { (void)e; });
    xpc_connection_activate(c);
    xpc_object_t m = xpc_dictionary_create(NULL, NULL, 0);
    __block bool ok = false;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    xpc_connection_send_message_with_reply(c, m, q, ^(xpc_object_t r) {
        ok = xpc_get_type(r) == XPC_TYPE_DICTIONARY && xpc_dictionary_get_bool(r, "ok");
        dispatch_semaphore_signal(done);
    });
    dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC));
    return ok;
}

int main(void) {
    char mine[41] = "";
    if (!own_fingerprint(mine)) {
        printf("FAILED: couldn't read own fingerprint\n");
        return 1;
    }
    // The app compares this value with the one the install script read using codesign.
    printf("fingerprint: %s\n", mine);
    char other[41];
    strlcpy(other, mine, sizeof other);
    other[0] = other[0] == '0' ? '1' : '0';
    bool same = answered_with(mine), different = answered_with(other);
    printf("matching fingerprint answered: %s\n", same ? "yes" : "no");
    printf("different fingerprint answered: %s\n", different ? "yes" : "no");
    return same && !different ? 0 : 1;
}
