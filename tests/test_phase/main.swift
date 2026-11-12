// SPDX-License-Identifier: GPL-3.0-or-later
// Checks that the panel shows the right thing for every important situation.
import Foundation

var failures = 0

func expect(_ facts: Phase.Facts, _ phase: Phase, line: Int = #line) {
    let got = Phase.from(facts)
    if got != phase {
        print("FAILED line \(line): got \(got), expected \(phase)")
        failures += 1
    }
}

var f = Phase.Facts()
expect(f, .noPhone)

f = Phase.Facts(phone: "Pixel 5")
expect(f, .ready("Pixel 5"))

f.startingConnect = true
expect(f, .connecting("Pixel 5"))

f = Phase.Facts(helperState: "connecting", helperPhone: "Pixel 5")
expect(f, .connecting("Pixel 5"))

f = Phase.Facts(helperState: "connected", helperPhone: "Pixel 5", phone: "Pixel 5")
expect(f, .connected("Pixel 5"))

f.startingDisconnect = true
expect(f, .disconnecting)

// A stale error must not hide a live connection.
f = Phase.Facts(helperState: "connected", helperPhone: "Pixel 5", error: "old")
expect(f, .connected("Pixel 5"))

f = Phase.Facts(phone: "Pixel 5", error: "No answer")
expect(f, .failed("No answer"))

// Setup steps come before everything else, and moving comes before setup.
f = Phase.Facts(inApplications: false, helper: .missing, phone: "Pixel 5")
expect(f, .needsMove)
f.inApplications = true
expect(f, .needsSetup)
f.helper = .outdated
expect(f, .needsUpdate)

// Every phase that needs the person to act has a button, and the rest explain themselves.
for phase in [Phase.needsMove, .needsSetup, .needsUpdate, .ready("x"), .connected("x"), .failed("x")] {
    if phase.button == nil { print("FAILED: \(phase) has no button"); failures += 1 }
}
for phase in [Phase.noPhone, .connecting("x"), .disconnecting] {
    if phase.button != nil { print("FAILED: \(phase) shouldn't have a button"); failures += 1 }
}

print(failures == 0 ? "all passed" : "\(failures) FAILED")
exit(failures == 0 ? 0 : 1)
