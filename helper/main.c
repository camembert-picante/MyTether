// SPDX-License-Identifier: GPL-3.0-or-later
// The root helper. macOS starts it when the app asks for something and it quits when idle,
// so it isn't running at all while no phone is connected. It keeps no log.
#include "net.h"
#include "tether.h"
#include <dispatch/dispatch.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xpc/xpc.h>

#define SERVICE "local.mytether.helperd"
#define PIN_FILE "/Library/PrivilegedHelperTools/" SERVICE "/app-cdhash"
#define ADDRESS_TIMEOUT_TICKS 100  // 30 seconds at 0.3 seconds per tick
#define IDLE_QUIT_SECONDS 30

typedef enum { IDLE, CONNECTING, CONNECTED } state_t;

static state_t state = IDLE;
static char last_error[160], address[46];
static xpc_connection_t client;
static unsigned generation;

static const char *state_name(void) {
    return state == CONNECTED ? "connected" : state == CONNECTING ? "connecting" : "idle";
}

static void fill_status(xpc_object_t d) {
    xpc_dictionary_set_string(d, "state", state_name());
    xpc_dictionary_set_string(d, "phone", state == IDLE ? "" : tether_phone());
    xpc_dictionary_set_string(d, "error", last_error);
    xpc_dictionary_set_string(d, "address", address);
    xpc_dictionary_set_uint64(d, "rx", state == IDLE ? 0 : tether_rx_bytes());
    xpc_dictionary_set_uint64(d, "tx", state == IDLE ? 0 : tether_tx_bytes());
}

static void push_status(void) {
    if (!client) return;
    xpc_object_t m = xpc_dictionary_create(NULL, NULL, 0);
    fill_status(m);
    xpc_connection_send_message(client, m);
    xpc_release(m);
}

static void quit_later_if_idle(void) {
    unsigned mine = ++generation;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, IDLE_QUIT_SECONDS * NSEC_PER_SEC),
                   dispatch_get_main_queue(), ^{
                       if (mine == generation && state == IDLE) exit(0);
                   });
}

static void disconnect(const char *error) {
    if (state == IDLE) return;
    tether_stop();
    state = IDLE;
    address[0] = 0;
    strlcpy(last_error, error ? error : "", sizeof last_error);
    quit_later_if_idle();
}

static void on_phone_gone(void) {
    dispatch_async(dispatch_get_main_queue(), ^{
        disconnect(NULL);
        push_status();
    });
}

// Connected means macOS got an address from the phone, because only then does the
// internet work. Until that happens the app keeps showing Connecting.
static void wait_for_address(unsigned session, int ticks) {
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 300 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
        if (session != generation || state != CONNECTING) return;
        if (net_address(address, sizeof address)) {
            state = CONNECTED;
            push_status();
        } else if (ticks > 0) {
            wait_for_address(session, ticks - 1);
        } else {
            disconnect("The phone connected but gave no internet address. Check that USB "
                       "tethering is on.");
            push_status();
        }
    });
}

static void connect_phone(bool prefer_phone) {
    if (state != IDLE) return;
    last_error[0] = 0;
    const char *error = tether_start(prefer_phone, on_phone_gone);
    if (error) {
        strlcpy(last_error, error, sizeof last_error);
        quit_later_if_idle();
        return;
    }
    state = CONNECTING;
    wait_for_address(++generation, ADDRESS_TIMEOUT_TICKS);
}

static void on_message(xpc_connection_t peer, xpc_object_t m) {
    if (xpc_get_type(m) == XPC_TYPE_ERROR) {
        if (peer == client) client = NULL;
        return;
    }
    if (xpc_get_type(m) != XPC_TYPE_DICTIONARY) return;
    client = peer;
    const char *command = xpc_dictionary_get_string(m, "command");
    bool prefer_phone = xpc_dictionary_get_bool(m, "prefer_phone");
    if (command && !strcmp(command, "connect")) {
        connect_phone(prefer_phone);
    } else if (command && !strcmp(command, "disconnect")) {
        disconnect(NULL);
    } else if (command && !strcmp(command, "prefer")) {
        if (state != IDLE) net_set_first(prefer_phone);
    }
    if (state == IDLE) quit_later_if_idle();

    xpc_object_t reply = xpc_dictionary_create_reply(m);
    if (reply) {
        fill_status(reply);
        xpc_connection_send_message(peer, reply);
        xpc_release(reply);
    }
}

// Set Up saved the fingerprint of the app it installed from. Only that exact build may
// talk to this helper, which is why connecting doesn't ask for a password every time.
static bool read_pin(char *requirement, size_t cap) {
    char hash[64] = "";
    FILE *f = fopen(PIN_FILE, "r");
    if (!f) return false;
    bool ok = fgets(hash, sizeof hash, f) != NULL;
    fclose(f);
    hash[strcspn(hash, "\r\n")] = 0;
    if (!ok || strlen(hash) != 40 || strspn(hash, "0123456789abcdef") != 40) return false;
    snprintf(requirement, cap, "cdhash H\"%s\"", hash);
    return true;
}

static void listen_for_app(void) {
    char requirement[96];
    if (!read_pin(requirement, sizeof requirement)) exit(1);
    xpc_connection_t listener = xpc_connection_create_mach_service(
        SERVICE, dispatch_get_main_queue(), XPC_CONNECTION_MACH_SERVICE_LISTENER);
    if (xpc_connection_set_peer_code_signing_requirement(listener, requirement) != 0) exit(1);
    xpc_connection_set_event_handler(listener, ^(xpc_object_t peer) {
        if (xpc_get_type(peer) != XPC_TYPE_CONNECTION) return;
        xpc_connection_set_event_handler(peer, ^(xpc_object_t m) {
            on_message(peer, m);
        });
        xpc_connection_activate(peer);
    });
    xpc_connection_activate(listener);
}

static void on_signal(int number, void (^action)(void)) {
    signal(number, SIG_IGN);
    dispatch_source_t s =
        dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, (uintptr_t)number, 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(s, action);
    dispatch_resume(s);
}

// Runs a full connection in the terminal, without the app or Set Up, for testing.
static int run_in_terminal(void) {
    if (geteuid() != 0) {
        printf("This needs sudo.\n");
        return 1;
    }
    void (^quit)(void) = ^{
        tether_stop();
        printf("\nDisconnected.\n");
        exit(0);
    };
    on_signal(SIGINT, quit);
    on_signal(SIGTERM, quit);
    const char *error = tether_start(false, on_phone_gone);
    if (error) {
        printf("%s\n", error);
        return 1;
    }
    state = CONNECTING;
    printf("Connecting to %s. Press Ctrl-C to stop.\n", tether_phone());
    dispatch_source_t timer =
        dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_timer(timer, DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC, NSEC_PER_SEC / 2);
    dispatch_source_set_event_handler(timer, ^{
        char now[46] = "";
        if (state == IDLE) {
            printf("The phone went away.\n");
            exit(0);
        }
        net_address(now, sizeof now);
        printf("address %s  received %llu  sent %llu\n", now[0] ? now : "(waiting)",
               (unsigned long long)tether_rx_bytes(), (unsigned long long)tether_tx_bytes());
    });
    dispatch_resume(timer);
    dispatch_main();
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--probe")) return tether_probe(argc > 2 ? atoi(argv[2]) : 5);
    if (argc > 1 && !strcmp(argv[1], "--run")) return run_in_terminal();

    // Anything still here belongs to a session that ended in a crash.
    net_remove_services();
    net_destroy();

    on_signal(SIGTERM, ^{
        disconnect(NULL);
        exit(0);
    });
    listen_for_app();
    quit_later_if_idle();
    dispatch_main();
}
