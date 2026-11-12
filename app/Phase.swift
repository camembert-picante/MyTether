// SPDX-License-Identifier: GPL-3.0-or-later
// What the panel shows at any moment. Worked out from facts in one place, so the panel can
// never show two conflicting states.
enum Phase: Equatable {
    case needsMove, needsSetup, needsUpdate
    case noPhone
    case ready(String)
    case connecting(String)
    case connected(String)
    case disconnecting
    case failed(String)

    struct Facts {
        var inApplications = true
        var helper = HelperInstall.current
        var helperState = "idle"
        var helperPhone = ""
        var phone: String?
        var error: String?
        var startingConnect = false
        var startingDisconnect = false
    }

    static func from(_ f: Facts) -> Phase {
        if !f.inApplications { return .needsMove }
        switch f.helper {
        case .missing: return .needsSetup
        case .outdated: return .needsUpdate
        case .current: break
        }
        let name = f.helperPhone.isEmpty ? (f.phone ?? "your phone") : f.helperPhone
        if f.startingDisconnect { return .disconnecting }
        if f.helperState == "connected" { return .connected(name) }
        if f.helperState == "connecting" || f.startingConnect { return .connecting(name) }
        if let error = f.error { return .failed(error) }
        if let phone = f.phone { return .ready(phone) }
        return .noPhone
    }

    var title: String {
        switch self {
        case .needsMove: return "Move to Applications"
        case .needsSetup: return "One quick setup"
        case .needsUpdate: return "Finish updating"
        case .noPhone: return "Plug in your phone"
        case .ready(let phone): return phone
        case .connecting: return "Connecting…"
        case .connected: return "Connected"
        case .disconnecting: return "Disconnecting…"
        case .failed: return "Couldn’t connect"
        }
    }

    var subtitle: String {
        switch self {
        case .needsMove: return "MyTether needs to run from your Applications folder."
        case .needsSetup: return "macOS will ask for your password, just this once."
        case .needsUpdate: return "This new version needs your password once."
        case .noPhone: return "Use a USB cable, then turn on USB tethering on the phone."
        case .ready: return "Ready to connect."
        case .connecting(let phone): return phone
        case .connected(let phone): return "Using \(phone)’s internet"
        case .disconnecting: return ""
        case .failed(let message): return message
        }
    }

    var button: String? {
        switch self {
        case .needsMove: return "Move to Applications"
        case .needsSetup: return "Set Up"
        case .needsUpdate: return "Update"
        case .ready: return "Connect"
        case .connected: return "Disconnect"
        case .failed: return "Try Again"
        case .noPhone, .connecting, .disconnecting: return nil
        }
    }

    var isConnected: Bool {
        if case .connected = self { return true }
        return false
    }

    var isWorking: Bool {
        switch self {
        case .connecting, .disconnecting: return true
        default: return false
        }
    }
}

enum HelperInstall: Equatable {
    case missing, outdated, current
}
