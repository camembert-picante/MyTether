// SPDX-License-Identifier: GPL-3.0-or-later
import AppKit
import Security

enum InstallResult {
    case done, cancelled, failed
}

enum Installer {
    private static let label = "local.mytether.helperd"
    private static let plist = "/Library/LaunchDaemons/\(label).plist"
    private static let pinFile = "/Library/PrivilegedHelperTools/\(label)/app-cdhash"
    private static let destination = URL(fileURLWithPath: "/Applications/MyTether.app")

    static var inApplications: Bool {
        Bundle.main.bundleURL.standardizedFileURL.path == destination.path
    }

    static var helper: HelperInstall {
        guard FileManager.default.fileExists(atPath: plist) else { return .missing }
        let pinned = (try? String(contentsOfFile: pinFile, encoding: .utf8))?
            .trimmingCharacters(in: .whitespacesAndNewlines)
        return pinned != nil && pinned == ownFingerprint ? .current : .outdated
    }

    /// The same fingerprint Set Up saved for the helper to check.
    private static var ownFingerprint: String? {
        var code: SecCode?
        var file: SecStaticCode?
        var info: CFDictionary?
        guard SecCodeCopySelf([], &code) == errSecSuccess, let code,
              SecCodeCopyStaticCode(code, [], &file) == errSecSuccess, let file,
              SecCodeCopySigningInformation(file, [], &info) == errSecSuccess,
              let info = info as? [String: Any],
              let hash = info[kSecCodeInfoUnique as String] as? Data else { return nil }
        return hash.map { String(format: "%02x", $0) }.joined()
    }

    /// Copies the app to Applications, quits any older copy, and opens the new one.
    static func moveToApplications() -> Bool {
        let files = FileManager.default
        let temporary = destination.deletingLastPathComponent().appendingPathComponent(".MyTether-new.app")
        do {
            try? files.removeItem(at: temporary)
            try files.copyItem(at: Bundle.main.bundleURL, to: temporary)
        } catch {
            return false
        }
        for other in NSRunningApplication.runningApplications(withBundleIdentifier: "local.mytether.app")
        where other != .current {
            other.terminate()
        }
        do {
            if files.fileExists(atPath: destination.path) {
                _ = try files.replaceItemAt(destination, withItemAt: temporary)
            } else {
                try files.moveItem(at: temporary, to: destination)
            }
        } catch {
            try? files.removeItem(at: temporary)
            return false
        }
        let configuration = NSWorkspace.OpenConfiguration()
        configuration.createsNewApplicationInstance = true
        NSWorkspace.shared.openApplication(at: destination, configuration: configuration) { _, _ in
            DispatchQueue.main.async { NSApp.terminate(nil) }
        }
        return true
    }

    static func installHelper() async -> InstallResult {
        await runAsAdmin(script: "install-helper.sh",
                         prompt: "MyTether needs your password once to finish setting up.")
    }

    static func removeHelper() async -> InstallResult {
        await runAsAdmin(script: "remove-helper.sh", prompt: "MyTether needs your password to remove its helper.")
    }

    /// osascript shows the standard macOS password dialog and runs the script as root.
    private static func runAsAdmin(script: String, prompt: String) async -> InstallResult {
        guard let path = Bundle.main.path(forResource: script, ofType: nil) else { return .failed }
        let source = """
        on run argv
            do shell script "/bin/sh " & quoted form of (item 1 of argv) & " " & quoted form of (item 2 of argv) ¬
                with prompt (item 3 of argv) with administrator privileges
        end run
        """
        let appPath = Bundle.main.bundlePath
        return await Task.detached {
            let process = Process()
            let errors = Pipe()
            process.executableURL = URL(fileURLWithPath: "/usr/bin/osascript")
            process.arguments = ["-e", source, path, appPath, prompt]
            process.standardError = errors
            process.standardOutput = FileHandle.nullDevice
            guard (try? process.run()) != nil else { return InstallResult.failed }
            let output = errors.fileHandleForReading.readDataToEndOfFile()
            process.waitUntilExit()
            if process.terminationStatus == 0 { return .done }
            return String(decoding: output, as: UTF8.self).contains("-128") ? .cancelled : .failed
        }.value
    }
}
