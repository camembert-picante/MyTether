// SPDX-License-Identifier: GPL-3.0-or-later
import AppKit
import ServiceManagement

@Observable @MainActor
final class Model {
    private var facts = Phase.Facts()
    var phase: Phase { Phase.from(facts) }
    private(set) var downloadSpeed = 0.0
    private(set) var uploadSpeed = 0.0
    private(set) var address = ""

    var autoConnect = UserDefaults.standard.object(forKey: "autoConnect") as? Bool ?? true {
        didSet { UserDefaults.standard.set(autoConnect, forKey: "autoConnect"); scheduleAutoConnect() }
    }
    var preferPhone = UserDefaults.standard.bool(forKey: "preferPhone") {
        didSet {
            UserDefaults.standard.set(preferPhone, forKey: "preferPhone")
            if phase.isConnected { helper.send("prefer", preferPhone: preferPhone) { _ in } }
        }
    }
    var showSpeedInMenuBar = UserDefaults.standard.bool(forKey: "showSpeedInMenuBar") {
        didSet { UserDefaults.standard.set(showSpeedInMenuBar, forKey: "showSpeedInMenuBar"); updateSpeedUpdates() }
    }
    var startAtLogin = SMAppService.mainApp.status == .enabled {
        didSet {
            guard startAtLogin != (SMAppService.mainApp.status == .enabled) else { return }
            if startAtLogin {
                try? SMAppService.mainApp.register()
            } else {
                try? SMAppService.mainApp.unregister()
            }
        }
    }
    var panelOpen = false {
        didSet { updateSpeedUpdates() }
    }

    private let helper = HelperClient()
    private var started = false
    /// Stops auto-connect from undoing a Disconnect, or retrying a failure, until the phone
    /// is unplugged.
    private var holdOffAutoConnect = false
    private var autoConnectTask: Task<Void, Never>?
    private var speedTask: Task<Void, Never>?

    func start() {
        guard !started else { return }
        started = true
        facts.inApplications = Installer.inApplications
        facts.helper = Installer.helper
        helper.onStatus = { [weak self] status in
            MainActor.assumeIsolated { self?.apply(status) }
        }
        phone_watch({ context in
            MainActor.assumeIsolated {
                Unmanaged<Model>.fromOpaque(context!).takeUnretainedValue().phoneListChanged()
            }
        }, Unmanaged.passUnretained(self).toOpaque())
        readPhone()
        if facts.helper == .current {
            helper.send("status") { [weak self] status in
                if let status { MainActor.assumeIsolated { self?.apply(status) } }
                MainActor.assumeIsolated { self?.scheduleAutoConnect() }
            }
        }
        offerFirstStep()
    }

    /// There's no window, so a person who just opened the app needs to be told what to do.
    private func offerFirstStep() {
        guard let button = phase.button, !isConnection(phase) else { return }
        NSApp.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        alert.icon = NSImage(named: NSImage.applicationIconName)
        alert.messageText = phase.title
        alert.informativeText = phase.subtitle
        alert.addButton(withTitle: button)
        alert.addButton(withTitle: "Not Now")
        if alert.runModal() == .alertFirstButtonReturn { primaryAction() }
    }

    private func isConnection(_ phase: Phase) -> Bool {
        switch phase {
        case .needsMove, .needsSetup, .needsUpdate: return false
        default: return true
        }
    }

    func primaryAction() {
        switch phase {
        case .needsMove: move()
        case .needsSetup, .needsUpdate: Task { await setUp() }
        case .ready, .failed:
            facts.error = nil
            holdOffAutoConnect = false
            connect()
        case .connected: disconnect()
        default: break
        }
    }

    private func move() {
        guard Installer.moveToApplications() else {
            tell("Couldn’t move MyTether", "Drag MyTether into your Applications folder, then open it from there.")
            return
        }
    }

    private func setUp() async {
        switch await Installer.installHelper() {
        case .done:
            facts.helper = Installer.helper
            tell("You’re all set", "MyTether lives in the menu bar, at the top of your screen. "
                + "Look for the small plug.")
            scheduleAutoConnect()
        case .failed:
            tell("Setup didn’t finish", "Click Set Up to try again.")
        case .cancelled:
            break
        }
    }

    func removeHelper() {
        NSApp.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        alert.messageText = "Remove the background helper?"
        alert.informativeText = "Any connection ends. To remove MyTether completely, drag it to the Trash afterwards."
        alert.addButton(withTitle: "Remove")
        alert.addButton(withTitle: "Cancel")
        guard alert.runModal() == .alertFirstButtonReturn else { return }
        Task {
            if await Installer.removeHelper() == .done {
                apply(HelperStatus())
                facts.helper = Installer.helper
            }
        }
    }

    func quit() {
        guard phase.isConnected || facts.helperState == "connecting" else { NSApp.terminate(nil); return }
        helper.send("disconnect") { _ in NSApp.terminate(nil) }
    }

    private func connect() {
        facts.startingConnect = true
        helper.send("connect", preferPhone: preferPhone) { [weak self] status in
            MainActor.assumeIsolated {
                guard let self else { return }
                self.facts.startingConnect = false
                if let status {
                    self.apply(status)
                } else {
                    self.facts.error = "The background helper didn’t answer. Try again, or remove it "
                        + "from the gear menu and set it up again."
                    self.holdOffAutoConnect = true
                }
            }
        }
    }

    private func disconnect() {
        holdOffAutoConnect = true
        facts.startingDisconnect = true
        helper.send("disconnect") { [weak self] status in
            MainActor.assumeIsolated {
                self?.facts.startingDisconnect = false
                self?.apply(status ?? HelperStatus())
            }
        }
    }

    private func apply(_ status: HelperStatus) {
        facts.helperState = status.state
        facts.helperPhone = status.phone
        address = status.address
        if !status.error.isEmpty {
            facts.error = status.error
            holdOffAutoConnect = true
        }
        updateSpeedUpdates()
        if status.state == "idle" { scheduleAutoConnect() }
    }

    private func readPhone() {
        var name = [CChar](repeating: 0, count: 128)
        facts.phone = phone_find(&name, name.count, nil, nil) ? String(cString: name) : nil
    }

    fileprivate func phoneListChanged() {
        readPhone()
        if facts.phone == nil {
            facts.error = nil
            holdOffAutoConnect = false
        }
        scheduleAutoConnect()
    }

    /// A phone's USB parts appear one at a time, so this waits for them all to arrive.
    private func scheduleAutoConnect() {
        autoConnectTask?.cancel()
        guard autoConnect, !holdOffAutoConnect, case .ready = phase else { return }
        autoConnectTask = Task {
            try? await Task.sleep(for: .seconds(1))
            guard !Task.isCancelled, autoConnect, !holdOffAutoConnect, case .ready = phase else { return }
            connect()
        }
    }

    /// Speeds are only asked for while someone can see them.
    private func updateSpeedUpdates() {
        let needed = phase.isConnected && (panelOpen || showSpeedInMenuBar)
        guard needed != (speedTask != nil) else { return }
        speedTask?.cancel()
        speedTask = nil
        downloadSpeed = 0
        uploadSpeed = 0
        guard needed else { return }
        speedTask = Task { await measureSpeeds() }
    }

    private func measureSpeeds() async {
        var last: (rx: UInt64, tx: UInt64, time: ContinuousClock.Instant)?
        while !Task.isCancelled {
            helper.send("status") { [weak self] status in
                guard let status else { return }
                MainActor.assumeIsolated {
                    guard let self, status.state == "connected" else { return }
                    let now = ContinuousClock.now
                    if let last {
                        let seconds = max((now - last.time) / .seconds(1), 0.1)
                        self.downloadSpeed = Double(status.rx &- last.rx) * 8 / seconds
                        self.uploadSpeed = Double(status.tx &- last.tx) * 8 / seconds
                    }
                    last = (status.rx, status.tx, now)
                }
            }
            try? await Task.sleep(for: .seconds(2))
        }
    }

    private func tell(_ title: String, _ message: String) {
        NSApp.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        alert.messageText = title
        alert.informativeText = message
        alert.runModal()
    }
}
