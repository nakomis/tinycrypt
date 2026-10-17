import SwiftUI

/// Gives a key its Wi-Fi: the networks it can hear, a password, then waits until it's online.
struct WifiSetupView: View {
    let key: KnownKey
    let onDone: () -> Void

    @State private var networks: [WifiNetwork] = []
    @State private var scanning = false
    @State private var chosen: WifiNetwork?
    @State private var otherName = ""
    @State private var password = ""
    @State private var joining = false
    @State private var status: String?
    @State private var online = false
    @FocusState private var passwordFocused: Bool
    @State private var showPassword = false

    var body: some View {
        Form {
            if online {
                Section {
                    Label("Your key is online.", systemImage: "checkmark.circle.fill").foregroundStyle(.green)
                    Button("Done", action: onDone)
                }
            } else if let chosen {
                Section {
                    // No content type: it's a Wi-Fi password, not a website login for the password manager.
                    HStack {
                        Group {
                            if showPassword {
                                TextField("Wi-Fi password", text: $password)
                            } else {
                                SecureField("Wi-Fi password", text: $password)
                            }
                        }
                        .autocorrectionDisabled()
                        .textInputAutocapitalization(.never)
                        .focused($passwordFocused)
                        .disabled(joining)
                        Button { toggleShowPassword() } label: {
                            Image(systemName: showPassword ? "eye.slash" : "eye").foregroundStyle(.secondary)
                        }
                        .buttonStyle(.borderless)
                        .accessibilityLabel(showPassword ? "Hide password" : "Show password")
                    }
                    .onAppear { passwordFocused = true }
                    Button(showPassword ? "Hide password" : "Show password") { toggleShowPassword() }
                        .buttonStyle(.borderless)
                        .font(.footnote)
                } header: {
                    Text("Password for \(chosen.ssid)")
                }
                Section {
                    VStack(spacing: 16) {
                        Button { Task { await join(chosen.ssid) } } label: {
                            Text(joining ? "Joining…" : "Join").frame(maxWidth: .infinity)
                        }
                        .buttonStyle(.borderedProminent)
                        .controlSize(.large)
                        // WPA2 needs 8 or more; an open network (or one typed in by name) may have none.
                        .disabled(joining || (chosen.secured ? password.count < 8 : (1...7).contains(password.count)))
                        Button("Choose another network") { self.chosen = nil; status = nil }
                            .buttonStyle(.borderless)
                            .disabled(joining)
                    }
                    .frame(maxWidth: .infinity)
                }
                .listRowBackground(Color.clear)
                if let status { Section { Text(status) } }
            } else {
                Section {
                    if scanning {
                        ProgressView("Looking for networks…")
                    }
                    ForEach(networks) { net in
                        Button { chosen = net; password = "" } label: {
                            HStack {
                                Text(net.ssid)
                                Spacer()
                                if net.secured { Image(systemName: "lock.fill").foregroundStyle(.secondary) }
                                Image(systemName: "wifi", variableValue: Self.strength(net.rssi))
                            }
                        }.foregroundStyle(.primary)
                    }
                    Button("Look again") { Task { await scan() } }.disabled(scanning)
                } header: {
                    Text("Networks your key can hear")
                } footer: {
                    Text("Your key needs Wi-Fi to tell your phone when it wants approval. The button works without it.")
                }
                Section("Not listed?") {
                    TextField("Network name", text: $otherName).autocorrectionDisabled().textInputAutocapitalization(.never)
                    Button("Use this network") { chosen = WifiNetwork(ssid: otherName, rssi: -100, secured: false) }
                        .disabled(otherName.isEmpty)
                }
                if let status { Section { Text(status) } }
                Section { Button("Skip for now", action: onDone) }
            }
        }
        .task { await scan() }
    }

    /// Swapping between the secure and plain fields would drop the keyboard; give it back.
    private func toggleShowPassword() {
        showPassword.toggle()
        if !joining { passwordFocused = true }
    }

    private func scan() async {
        scanning = true
        defer { scanning = false }
        do {
            networks = try await KeyActions.scanWifi(key: key.peripheralID)
            status = networks.isEmpty ? "The key didn't hear any networks. Try Look again." : nil
        } catch {
            status = error.localizedDescription
        }
    }

    private func join(_ ssid: String) async {
        joining = true
        passwordFocused = false
        // The password stays (greyed out) while the key tries it, and goes once it has worked or failed.
        defer {
            joining = false
            password = ""
        }
        status = "Sending to your key…"
        do {
            try await KeyActions.setWifi(key: key.peripheralID, ssid: ssid, password: password)
        } catch {
            status = error.localizedDescription
            return
        }
        // Joining and then reaching AWS takes a few seconds; the key says how it's going.
        for _ in 0..<15 {
            try? await Task.sleep(for: .seconds(3))
            guard let info = try? await KeyActions.info(key: key.peripheralID) else { continue }
            status = Self.describe(info)
            if info.iot { online = true; return }
            if info.wifi.state == "wrong-password" || info.wifi.state == "not-found" { return }
        }
        status = "Still not online. Check the network, or try again later: the key keeps trying."
    }

    static func describe(_ info: KeyInfo) -> String {
        switch info.wifi.state {
        case "connected": return info.iot ? "Online" : "On Wi-Fi, reaching the internet…"
        case "connecting": return "Joining \(info.wifi.ssid)…"
        case "wrong-password": return "Wrong password for \(info.wifi.ssid)."
        case "not-found": return "Can't find \(info.wifi.ssid). Is the key in range?"
        case "unset": return "No Wi-Fi set"
        default: return "Trying to join \(info.wifi.ssid)…"
        }
    }

    static func strength(_ rssi: Int) -> Double {
        rssi >= -55 ? 1 : rssi >= -70 ? 0.66 : 0.33
    }
}
