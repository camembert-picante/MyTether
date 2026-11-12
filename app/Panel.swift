// SPDX-License-Identifier: GPL-3.0-or-later
import AppKit
import SwiftUI

@main
struct MyTetherApp: App {
    @State private var model = Model()

    var body: some Scene {
        MenuBarExtra {
            Panel(model: model)
        } label: {
            HStack(spacing: 4) {
                Image(nsImage: usbMenuBarImage())
                if model.showSpeedInMenuBar, model.phase.isConnected {
                    Text(speedText(model.downloadSpeed)).monospacedDigit()
                }
            }
            .task { model.start() }
        }
        .menuBarExtraStyle(.window)
    }
}

/// Everything MyTether shows lives in this one panel.
struct Panel: View {
    @Bindable var model: Model

    var body: some View {
        let phase = model.phase
        VStack(alignment: .leading, spacing: 12) {
            HStack(alignment: .top, spacing: 12) {
                StatusBadge(tint: tint(phase), working: phase.isWorking)
                VStack(alignment: .leading, spacing: 2) {
                    Text(phase.title).font(.headline).lineLimit(1)
                    if !phase.subtitle.isEmpty {
                        Text(phase.subtitle)
                            .font(.caption)
                            .foregroundStyle(.secondary)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }
                Spacer(minLength: 0)
                SettingsGear(model: model)
            }

            if phase.isConnected {
                HStack {
                    Label(speedText(model.downloadSpeed), systemImage: "arrow.down")
                    Spacer()
                    Label(speedText(model.uploadSpeed), systemImage: "arrow.up")
                }
                .font(.callout.monospacedDigit())
            }

            if phase == .noPhone {
                Text("On most phones: **Settings › Network & internet › Hotspot & tethering › USB tethering**")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .padding(8)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .background(.quaternary.opacity(0.5), in: RoundedRectangle(cornerRadius: 8))
            }

            if let title = phase.button {
                Button { model.primaryAction() } label: {
                    Text(title).frame(maxWidth: .infinity)
                }
                .controlSize(.large)
                .modifier(ButtonEmphasis(strong: !phase.isConnected))
            }
        }
        .padding(14)
        .frame(width: 290)
        .animation(.smooth(duration: 0.2), value: phase)
        .onAppear { model.panelOpen = true }
        .onDisappear { model.panelOpen = false }
    }

    /// Green when connected, gray when there's no phone or the connection failed, blue the
    /// rest of the time.
    private func tint(_ phase: Phase) -> Color {
        switch phase {
        case .connected: return .connectedGreen
        case .noPhone, .failed: return .idleGray
        default: return .plugBlue
        }
    }
}

/// One palette, built on the app icon's blue.
extension Color {
    /// The blue for the badge and the main button. Dark mode uses the icon's blue as is. Light
    /// mode takes a slightly deeper shade, the lightest one that white text can sit on (4.6:1).
    static let plugBlue = Color(light: 0x2474D6, dark: 0x3A9BFA)
    static let connectedGreen = Color(light: 0x15924F, dark: 0x3DD68C)
    /// A light gray, just dark enough to keep the plug visible on its circle (3.1:1).
    static let idleGray = Color(light: 0x85858B, dark: 0xA1A1A6)

    /// Main button text: white in light mode, midnight on the brighter dark-mode blue (6.5:1).
    static let buttonText = Color(light: 0xFFFFFF, dark: 0x0B1022)
    /// Text and soft fill for the quieter Disconnect button. Each mode uses a blue that reads
    /// as text on its own tint (5:1 or better).
    static let accentText = Color(light: 0x1A56A8, dark: 0x8CC4FF)

    init(hex: UInt32) {
        self.init(nsColor: NSColor(hex: hex))
    }

    init(light: UInt32, dark: UInt32) {
        self.init(nsColor: NSColor(name: nil) { appearance in
            NSColor(hex: appearance.bestMatch(from: [.aqua, .darkAqua]) == .darkAqua ? dark : light)
        })
    }
}

private extension NSColor {
    convenience init(hex: UInt32) {
        self.init(srgbRed: CGFloat(hex >> 16 & 0xFF) / 255, green: CGFloat(hex >> 8 & 0xFF) / 255,
                  blue: CGFloat(hex & 0xFF) / 255, alpha: 1)
    }
}

private struct ButtonEmphasis: ViewModifier {
    var strong: Bool

    func body(content: Content) -> some View {
        // Return only triggers forward steps, so it can't disconnect anyone by accident.
        if strong {
            content.buttonStyle(PanelButtonStyle(strong: true)).keyboardShortcut(.defaultAction)
        } else {
            content.buttonStyle(PanelButtonStyle(strong: false))
        }
    }
}

/// Both buttons share one capsule. The main one is filled; Disconnect gets a soft tint so
/// it reads as the quieter choice.
private struct PanelButtonStyle: ButtonStyle {
    var strong: Bool

    func makeBody(configuration: Configuration) -> some View {
        let fill = strong ? Color.plugBlue : Color.accentText.opacity(0.12)
        configuration.label
            .font(.body.weight(.medium))
            .foregroundStyle(strong ? Color.buttonText : Color.accentText)
            .frame(minHeight: 28)
            .background(fill, in: Capsule())
            .opacity(configuration.isPressed ? 0.75 : 1)
            .contentShape(Capsule())
    }
}

private struct SettingsGear: View {
    @Bindable var model: Model

    var body: some View {
        Menu {
            Toggle("Connect automatically", isOn: $model.autoConnect)
            Toggle("Prefer phone over Wi-Fi", isOn: $model.preferPhone)
            Toggle("Show speed in menu bar", isOn: $model.showSpeedInMenuBar)
            Toggle("Start at login", isOn: $model.startAtLogin)
            Divider()
            if Installer.helper != .missing {
                Button("Remove Background Helper…") { model.removeHelper() }
            }
            Button("Quit MyTether") { model.quit() }
        } label: {
            Image(systemName: "gearshape").foregroundStyle(.secondary)
        }
        .menuStyle(.borderlessButton)
        .menuIndicator(.hidden)
        .fixedSize()
        .help("Settings")
    }
}

/// The plug in a soft circle of the same color. A ring pulses while something is happening.
private struct StatusBadge: View {
    var tint: Color
    var working: Bool
    @State private var pulse = false

    var body: some View {
        ZStack {
            Circle()
                .stroke(tint.opacity(working ? 0.4 : 0), lineWidth: 2)
                .scaleEffect(pulse ? 1.15 : 1)
                .opacity(pulse ? 0 : 1)
            Circle().fill(tint.opacity(0.15))
            USBShape().fill(tint).padding(9)
        }
        .frame(width: 40, height: 40)
        .onChange(of: working, initial: true) {
            pulse = false
            guard working else { return }
            withAnimation(.easeOut(duration: 1.4).repeatForever(autoreverses: false)) { pulse = true }
        }
        .accessibilityHidden(true)
    }
}

func speedText(_ bitsPerSecond: Double) -> String {
    if bitsPerSecond >= 1_000_000 { return String(format: "%.1f Mbps", bitsPerSecond / 1_000_000) }
    return String(format: "%.0f Kbps", bitsPerSecond / 1_000)
}

private struct USBShape: Shape {
    func path(in rect: CGRect) -> Path { Path(usbGlyphPath(in: rect)) }
}

/// A template image, so macOS colors it to suit a light or dark menu bar.
private func usbMenuBarImage() -> NSImage {
    let image = NSImage(size: NSSize(width: 18, height: 18), flipped: true) { rect in
        guard let context = NSGraphicsContext.current?.cgContext else { return false }
        // Half a point of margin keeps the edges from touching neighbouring menu bar icons.
        context.addPath(usbGlyphPath(in: rect.insetBy(dx: 0.5, dy: 0.5)))
        context.setFillColor(NSColor.black.cgColor)
        context.fillPath()
        return true
    }
    image.isTemplate = true
    image.accessibilityDescription = "MyTether"
    return image
}
