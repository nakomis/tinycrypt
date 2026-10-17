import SwiftUI
import UserNotifications

struct KeysView: View {
    @EnvironmentObject private var model: KeysModel
    @State private var adding = false

    var body: some View {
        NavigationStack {
            List {
                if !model.notificationsAllowed {
                    Section {
                        Label("Notifications are off, so this phone can't ask you to approve sign-ins.",
                              systemImage: "bell.slash")
                        Button("Turn them on in Settings") {
                            if let url = URL(string: UIApplication.openNotificationSettingsURLString) {
                                UIApplication.shared.open(url)
                            }
                        }
                    }
                }
                if model.keys.isEmpty {
                    Section {
                        Text("No keys yet. Plug your tinycrypt in, then tap Add a key.")
                            .foregroundStyle(.secondary)
                    }
                }
                ForEach(model.keys) { key in
                    NavigationLink(value: key) {
                        if model.needsSetup(key) {
                            Label { Text(key.name) + Text("  needs adding again").foregroundStyle(.orange) }
                                icon: { Image(systemName: "key.horizontal") }
                        } else {
                            Label(key.name, systemImage: "key.horizontal")
                        }
                    }
                }
                Section {
                    Button { adding = true } label: { Label("Add a key", systemImage: "plus.circle") }
                }
                Section {
                    Text("When a site asks for your key, approve the notification here, or press the button on the key.")
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                }
            }
            .navigationTitle("tinycrypt")
            .navigationDestination(for: KnownKey.self) { KeyDetailView(key: $0) }
            .sheet(isPresented: $adding) { AddKeyView() }
            .fullScreenCover(item: $model.pending) { pending in
                ApproveView(pending: pending) { model.pending = nil }
            }
            .task {
                await model.refreshNotificationPermission()
                await model.reregister()
            }
            .onReceive(NotificationCenter.default.publisher(for: UIApplication.willEnterForegroundNotification)) { _ in
                Task { await model.refreshNotificationPermission() }
            }
            .alert(model.notice ?? "", isPresented: Binding(get: { model.notice != nil },
                                                             set: { if !$0 { model.notice = nil } })) {
                Button("OK") { model.notice = nil }
            }
        }
    }
}

/// Adding a key: enrol this phone, then give the key its Wi-Fi.
struct AddKeyView: View {
    @EnvironmentObject private var model: KeysModel
    @Environment(\.dismiss) private var dismiss
    @State private var stage = Stage.instructions
    @State private var added: KnownKey?

    enum Stage: Equatable { case instructions, enrolling, wifi, failed(String) }

    var body: some View {
        NavigationStack {
            Group {
                switch stage {
                case .instructions:
                    instructions
                case .enrolling:
                    ProgressView("Connecting to your key…\nIf your phone asks to pair, tap Pair.")
                        .multilineTextAlignment(.center)
                case .wifi:
                    if let added { WifiSetupView(key: added, onDone: { dismiss() }) }
                case .failed(let why):
                    VStack(spacing: 16) {
                        Image(systemName: "exclamationmark.triangle").font(.largeTitle).foregroundStyle(.orange)
                        Text(why).multilineTextAlignment(.center)
                        Button("Try again") { stage = .instructions }.buttonStyle(.borderedProminent)
                    }.padding()
                }
            }
            .navigationTitle("Add a key")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } } }
        }
    }

    private var instructions: some View {
        VStack(alignment: .leading, spacing: 20) {
            Step(n: 1, text: "Plug your key in.")
            Step(n: 2, text: "Hold its button for 5 seconds, until the light pulses magenta.")
            Step(n: 3, text: "Keep the key close to this phone, then tap Continue. Allow notifications when asked: that's how the key asks you to approve.")
            Spacer()
            HStack(spacing: 10) {
                Text("Is the key pulsing magenta?").font(.headline)
                PulsingDot()
            }
            .frame(maxWidth: .infinity)
            Button { Task { await enrol() } } label: {
                Text("Continue").frame(maxWidth: .infinity)
            }.buttonStyle(.borderedProminent).controlSize(.large)
        }.padding()
    }

    private func enrol() async {
        stage = .enrolling
        // Asked here, where it's clear why: without it the phone can't prompt for approvals.
        _ = try? await UNUserNotificationCenter.current().requestAuthorization(options: [.alert, .sound])
        await model.refreshNotificationPermission()
        do {
            let (info, id, fingerprint) = try await KeyActions.enrol(as: .phone)
            // Saved as soon as the key has enrolled us; registering for its pushes is retried until it works.
            let key = KnownKey(thing: info.thing, peripheralID: id, registeredToken: nil, fingerprint: fingerprint)
            model.save(key)
            added = key
            await model.reregister()
            stage = .wifi
        } catch {
            stage = .failed(error.localizedDescription)
        }
    }
}

/// The key's enrol light: magenta, breathing.
private struct PulsingDot: View {
    @State private var bright = false
    var body: some View {
        Circle()
            .fill(Color(red: 1, green: 0, blue: 1))
            .frame(width: 18, height: 18)
            .opacity(bright ? 1 : 0.2)
            .animation(.easeInOut(duration: 1).repeatForever(autoreverses: true), value: bright)
            .onAppear { bright = true }
            .accessibilityLabel("pulsing magenta light")
    }
}

private struct Step: View {
    let n: Int
    let text: String
    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            Text("\(n)").font(.headline).frame(width: 28, height: 28).background(Circle().fill(.tint.opacity(0.2)))
            Text(text)
        }
    }
}

struct KeyDetailView: View {
    @EnvironmentObject private var model: KeysModel
    @Environment(\.dismiss) private var dismiss
    let key: KnownKey
    @State private var info: KeyInfo?
    @State private var error: String?
    @State private var changingWifi = false
    @State private var confirmingForget = false

    var body: some View {
        List {
            Section("Status") {
                if let info {
                    LabeledContent("Wi-Fi", value: info.wifi.ssid.isEmpty ? "not set" : info.wifi.ssid)
                    LabeledContent("Connection", value: WifiSetupView.describe(info))
                    LabeledContent("Firmware", value: info.version)
                    LabeledContent("Approvers", value: [info.watch ? "watch" : nil, info.phone ? "phone" : nil]
                        .compactMap { $0 }.joined(separator: " and "))
                } else if let error {
                    Text(error).foregroundStyle(.secondary)
                } else {
                    ProgressView()
                }
                Button("Refresh") { Task { await refresh() } }
            }
            Section {
                Button("Change Wi-Fi") { changingWifi = true }
            }
            Section {
                Button("Forget this key", role: .destructive) { confirmingForget = true }
            } footer: {
                Text("Only on this phone. To stop this phone approving for the key, enrol another phone with it.")
            }
        }
        .navigationTitle(key.name)
        .task { await refresh() }
        .sheet(isPresented: $changingWifi) {
            NavigationStack {
                WifiSetupView(key: key, onDone: { changingWifi = false; Task { await refresh() } })
                    .navigationTitle("Wi-Fi").navigationBarTitleDisplayMode(.inline)
            }
        }
        .confirmationDialog("Forget \(key.name)?", isPresented: $confirmingForget) {
            Button("Forget", role: .destructive) { model.forget(key); dismiss() }
        }
    }

    private func refresh() async {
        error = nil
        do {
            info = try await KeyActions.info(key: key.peripheralID)
        } catch {
            self.error = "Couldn't reach the key. Is it plugged in and nearby?"
        }
    }
}
