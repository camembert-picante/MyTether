// SPDX-License-Identifier: GPL-3.0-or-later
#include "tether.h"
#include "net.h"
#include "rndis.h"
#include "usb.h"
#include <dispatch/dispatch.h>
#include <errno.h>
#include <net/bpf.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/qos.h>
#include <unistd.h>

#define RX_SLOTS 4
#define TRANSFER_SIZE 16384
#define CONTROL_SIZE 1024

static const char *const NO_ANSWER = "The phone didn't answer. Unplug it and try again.";
static const char *const NO_NETWORK = "Couldn't set up the network connection.";

static struct {
    usb_link usb;
    rndis_caps caps;
    uint8_t mac[6];
    char phone[128];
    int packets;
    uint32_t packets_len;
    uint8_t *packets_buf;
    pthread_t usb_thread, send_thread;
    bool usb_running, send_running;
    _Atomic(CFRunLoopRef) loop;
    atomic_bool stop, usb_done;
    atomic_bool write_pending, sender_active;
    _Atomic IOReturn write_result;
    dispatch_semaphore_t usb_ready, write_done;
    atomic_uint_fast64_t rx_bytes, tx_bytes, rx_frames;
    int rx_pending;  // only touched on the USB thread
    void (*ended)(void);
} t = {.packets = -1};

static uint8_t rx_buffers[RX_SLOTS][TRANSFER_SIZE];
static uint8_t tx_buffer[TRANSFER_SIZE + 1];

const char *tether_phone(void) { return t.phone; }
uint64_t tether_rx_bytes(void) { return atomic_load_explicit(&t.rx_bytes, memory_order_relaxed); }
uint64_t tether_tx_bytes(void) { return atomic_load_explicit(&t.tx_bytes, memory_order_relaxed); }

static void session_failed(void) {
    if (!atomic_exchange(&t.stop, true) && t.ended) t.ended();
}

// Sends one request and waits for its reply. The phone rejects the read until the reply
// is ready, so this retries instead of watching USB for a "reply ready" signal.
static bool ask(void *request, size_t len, uint32_t id, uint8_t *reply, size_t *got) {
    if (usb_command(&t.usb, request, len) != kIOReturnSuccess) return false;
    useconds_t wait = 1000;
    for (useconds_t waited = 0; waited < 2000000; waited += wait) {
        IOReturn r = usb_response(&t.usb, reply, CONTROL_SIZE, got);
        if (r == kIOReturnNoDevice) return false;
        if (r == kIOReturnSuccess && rndis_is_reply(reply, *got, id)) return true;
        usleep(wait);
        if (wait < 20000) wait *= 2;
    }
    return false;
}

static const char *handshake(void) {
    uint8_t request[64], reply[CONTROL_SIZE];
    size_t got, n, size;
    const uint8_t *data;

    n = rndis_build_init(request, 1, TRANSFER_SIZE);
    if (!ask(request, n, 1, reply, &got) || !rndis_read_init(reply, got, &t.caps)) return NO_ANSWER;

    n = rndis_build_query(request, 2, RNDIS_OID_MAC);
    if (!ask(request, n, 2, reply, &got) || !rndis_read_query(reply, got, &data, &size) || size < 6)
        return NO_ANSWER;
    memcpy(t.mac, data, 6);

    n = rndis_build_set_u32(request, 3, RNDIS_OID_FILTER, RNDIS_FILTER_NORMAL);
    if (!ask(request, n, 3, reply, &got) || !rndis_read_set(reply, got)) return NO_ANSWER;
    return NULL;
}

static void halt(void) {
    uint8_t request[16];
    size_t n = rndis_build_halt(request, 4);
    usb_command(&t.usb, request, n);
}

static void on_frame(const uint8_t *frame, size_t len, void *ctx) {
    (void)ctx;
    atomic_fetch_add_explicit(&t.rx_frames, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&t.rx_bytes, len, memory_order_relaxed);
    // A failed write only loses this frame, and the protocols above resend it.
    if (t.packets >= 0) (void)!write(t.packets, frame, len);
}

static void on_read(void *slot, IOReturn result, void *size) {
    t.rx_pending--;
    if (atomic_load(&t.stop)) return;
    if (result == kIOReturnSuccess || result == kIOReturnUnderrun) {
        rndis_unpack(slot, (size_t)size, on_frame, NULL);
    } else if (result == kIOUSBPipeStalled) {
        usb_clear_in(&t.usb);
    } else if (result != kIOReturnOverrun) {
        session_failed();  // normally the phone was unplugged
        return;
    }
    if (usb_read(&t.usb, slot, TRANSFER_SIZE, on_read, slot) == kIOReturnSuccess) t.rx_pending++;
    else session_failed();
}

static void on_written(void *ctx, IOReturn result, void *size) {
    (void)ctx;
    (void)size;
    atomic_store(&t.write_result, result);
    atomic_store(&t.write_pending, false);
    dispatch_semaphore_signal(t.write_done);
}

// Runs USB completions. It sleeps until one arrives, so an idle connection costs nothing.
static void *usb_main(void *arg) {
    (void)arg;
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
    CFRunLoopRef loop = CFRunLoopGetCurrent();
    atomic_store(&t.loop, loop);
    if (usb_attach(&t.usb, loop) != kIOReturnSuccess) {
        session_failed();
    } else {
        for (int i = 0; i < RX_SLOTS; i++) {
            if (usb_read(&t.usb, rx_buffers[i], TRANSFER_SIZE, on_read, rx_buffers[i]) == kIOReturnSuccess)
                t.rx_pending++;
            else
                session_failed();
        }
    }
    dispatch_semaphore_signal(t.usb_ready);
    // The sender needs this thread to finish its last write, so this one leaves after it.
    while (!atomic_load(&t.stop) || t.rx_pending > 0 || atomic_load(&t.sender_active))
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 3600, true);
    atomic_store(&t.usb_done, true);
    return NULL;
}

static bool send_batch(rndis_batch *b) {
    size_t n = rndis_batch_finish(b);
    if (!n) return true;
    atomic_store(&t.write_pending, true);
    if (usb_write(&t.usb, tx_buffer, (uint32_t)n, on_written, NULL) != kIOReturnSuccess) {
        atomic_store(&t.write_pending, false);
        session_failed();
        return false;
    }
    // Only one write runs at a time. While it does, packets wait in the kernel's buffer
    // and the next transfer carries more of them.
    dispatch_time_t limit = dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC);
    if (dispatch_semaphore_wait(t.write_done, limit) != 0) {
        usb_abort_write(&t.usb);
        limit = dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC);
        if (dispatch_semaphore_wait(t.write_done, limit) != 0) _exit(1);  // USB stack is stuck
    }
    IOReturn r = atomic_load(&t.write_result);
    if (r == kIOReturnSuccess) return true;
    if (r == kIOUSBPipeStalled) {
        usb_clear_out(&t.usb);
        return true;
    }
    session_failed();
    return false;
}

// Moves packets from macOS to the phone.
static void *send_main(void *arg) {
    (void)arg;
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
    rndis_batch batch;
    // Stay one byte under the phone's limit so the end-of-transfer padding always fits.
    size_t cap = TRANSFER_SIZE;
    if (t.caps.max_transfer > 0 && t.caps.max_transfer - 1 < cap) cap = t.caps.max_transfer - 1;
    rndis_batch_init(&batch, tx_buffer, cap, &t.caps, t.usb.out_packet);

    while (!atomic_load(&t.stop)) {
        ssize_t got = read(t.packets, t.packets_buf, t.packets_len);
        if (got == 0 || (got < 0 && errno == EINTR)) continue;
        if (got < 0) {
            session_failed();
            break;
        }
        for (uint8_t *p = t.packets_buf, *end = p + got; p + sizeof(struct bpf_hdr) <= end;) {
            struct bpf_hdr *h = (struct bpf_hdr *)(void *)p;
            uint8_t *frame = p + h->bh_hdrlen;
            if (frame + h->bh_caplen > end) break;
            if (!rndis_batch_add(&batch, frame, h->bh_caplen)) {
                if (!send_batch(&batch)) goto done;
                if (!rndis_batch_add(&batch, frame, h->bh_caplen)) goto next;  // oversized: drop it
            }
            atomic_fetch_add_explicit(&t.tx_bytes, h->bh_caplen, memory_order_relaxed);
        next:
            p += BPF_WORDALIGN(h->bh_hdrlen + h->bh_caplen);
        }
        if (!send_batch(&batch)) break;
    }
done:
    atomic_store(&t.sender_active, false);
    return NULL;
}

static void reset_session(void (*ended)(void)) {
    t.ended = ended;
    t.packets = -1;
    t.rx_pending = 0;
    t.usb_running = t.send_running = false;
    atomic_store(&t.stop, false);
    atomic_store(&t.usb_done, false);
    atomic_store(&t.write_pending, false);
    atomic_store(&t.sender_active, false);
    atomic_store(&t.loop, NULL);
    atomic_store(&t.rx_bytes, 0);
    atomic_store(&t.tx_bytes, 0);
    atomic_store(&t.rx_frames, 0);
    if (!t.usb_ready) t.usb_ready = dispatch_semaphore_create(0);
    if (!t.write_done) t.write_done = dispatch_semaphore_create(0);
}

static bool start_usb_thread(void) {
    if (pthread_create(&t.usb_thread, NULL, usb_main, NULL) != 0) return false;
    t.usb_running = true;
    dispatch_semaphore_wait(t.usb_ready, DISPATCH_TIME_FOREVER);
    return true;
}

static void stop_usb_thread(void) {
    if (!t.usb_running) return;
    // Stop and abort repeat because a request made in the instant before the thread starts
    // waiting has no effect. If the USB stack never lets go, restarting this process is
    // the only clean way out, and launchd starts it again on the next request.
    for (int tries = 0; !atomic_load(&t.usb_done); tries++) {
        if (tries == 300) _exit(1);
        usb_abort(&t.usb);
        CFRunLoopRef loop = atomic_load(&t.loop);
        if (loop) CFRunLoopStop(loop);
        usleep(10000);
    }
    pthread_join(t.usb_thread, NULL);
    t.usb_running = false;
}

void tether_stop(void) {
    atomic_store(&t.stop, true);
    net_remove_services();
    // Removing the port also wakes the sending thread out of its read.
    net_destroy();
    if (t.usb.data) usb_abort(&t.usb);
    if (t.send_running) pthread_join(t.send_thread, NULL);
    t.send_running = false;
    stop_usb_thread();
    if (t.packets >= 0) close(t.packets);
    t.packets = -1;
    free(t.packets_buf);
    t.packets_buf = NULL;
    if (t.usb.comm) {
        halt();
        usb_close(&t.usb);
    }
}

const char *tether_start(bool prefer_phone, void (*ended)(void)) {
    reset_session(ended);
    const char *error = usb_open(&t.usb, t.phone, sizeof t.phone);
    if (error) return error;
    if (!(error = handshake()) && !net_create(t.mac)) error = NO_NETWORK;
    if (!error) {
        t.packets = net_open_packets(&t.packets_len);
        t.packets_buf = t.packets >= 0 ? malloc(t.packets_len) : NULL;
        if (!t.packets_buf || !net_add_service(prefer_phone)) error = NO_NETWORK;
    }
    if (!error) {
        // Marked before the USB thread starts, so that thread can't finish ahead of the sender.
        atomic_store(&t.sender_active, true);
        if (!start_usb_thread()) error = NO_NETWORK;
        if (!error && pthread_create(&t.send_thread, NULL, send_main, NULL) == 0) t.send_running = true;
        if (!t.send_running) {
            atomic_store(&t.sender_active, false);
            error = NO_NETWORK;
        }
    }
    if (error) {
        t.ended = NULL;
        tether_stop();
    }
    return error;
}

int tether_probe(int seconds) {
    reset_session(NULL);
    const char *error = usb_open(&t.usb, t.phone, sizeof t.phone);
    if (error) {
        printf("%s\n", error);
        return 1;
    }
    printf("Phone: %s\n", t.phone);
    printf("Claimed. Bulk pipes in=%u out=%u, USB packet %u bytes.\n", t.usb.in, t.usb.out,
           t.usb.out_packet);
    if ((error = handshake())) {
        printf("Handshake failed: %s\n", error);
        usb_close(&t.usb);
        return 1;
    }
    printf("Handshake done. Limits: %u bytes, %u frames, align %u.\n", t.caps.max_transfer,
           t.caps.max_packets, t.caps.align);
    printf("Address: %02x:%02x:%02x:%02x:%02x:%02x\n", t.mac[0], t.mac[1], t.mac[2], t.mac[3],
           t.mac[4], t.mac[5]);
    if (!start_usb_thread()) return 1;
    sleep((unsigned)seconds);
    bool phone_left = atomic_load(&t.stop);
    atomic_store(&t.stop, true);
    stop_usb_thread();
    halt();
    usb_close(&t.usb);
    printf("Received %llu frames, %llu bytes in %d seconds.\n",
           (unsigned long long)atomic_load(&t.rx_frames), (unsigned long long)atomic_load(&t.rx_bytes),
           seconds);
    printf(phone_left ? "The phone disconnected during the test.\n" : "Stopped cleanly.\n");
    return phone_left ? 1 : 0;
}
