// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation
import XPC

struct HelperStatus {
    var state = "idle"
    var phone = ""
    var error = ""
    var address = ""
    var rx: UInt64 = 0
    var tx: UInt64 = 0

    init() {}

    init(_ d: xpc_object_t) {
        func text(_ key: String) -> String {
            xpc_dictionary_get_string(d, key).map { String(cString: $0) } ?? ""
        }
        state = text("state")
        phone = text("phone")
        error = text("error")
        address = text("address")
        rx = xpc_dictionary_get_uint64(d, "rx")
        tx = xpc_dictionary_get_uint64(d, "tx")
    }
}

/// Talks to the root helper. macOS starts the helper when a message arrives.
final class HelperClient {
    /// The helper sends this when something changes by itself, like the phone being unplugged.
    var onStatus: ((HelperStatus) -> Void)?
    private var connection: xpc_connection_t?

    private func open() -> xpc_connection_t {
        if let connection { return connection }
        let c = xpc_connection_create_mach_service("local.mytether.helperd", .main,
                                                   UInt64(XPC_CONNECTION_MACH_SERVICE_PRIVILEGED))
        xpc_connection_set_event_handler(c) { [weak self] event in
            guard let self else { return }
            if xpc_get_type(event) == XPC_TYPE_DICTIONARY {
                self.onStatus?(HelperStatus(event))
            } else if self.connection === c {
                // The helper quits when idle. The next message starts it again.
                self.connection = nil
                xpc_connection_cancel(c)
                self.onStatus?(HelperStatus())
            }
        }
        xpc_connection_activate(c)
        connection = c
        return c
    }

    /// Calls reply on the main queue, with nil if the helper couldn't be reached. One retry
    /// covers the moment when the helper happens to be quitting as the message arrives.
    func send(_ command: String, preferPhone: Bool = false, retry: Bool = true,
              reply: @escaping (HelperStatus?) -> Void) {
        let message = xpc_dictionary_create(nil, nil, 0)
        xpc_dictionary_set_string(message, "command", command)
        xpc_dictionary_set_bool(message, "prefer_phone", preferPhone)
        xpc_connection_send_message_with_reply(open(), message, .main) { [weak self] answer in
            if xpc_get_type(answer) == XPC_TYPE_DICTIONARY {
                reply(HelperStatus(answer))
            } else if retry, let self {
                self.connection = nil
                self.send(command, preferPhone: preferPhone, retry: false, reply: reply)
            } else {
                reply(nil)
            }
        }
    }
}
