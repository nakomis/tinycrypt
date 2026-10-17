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

    var body: some View {
        Form {
            if online {
                Section {
                    Label("Your key is online.", systemImage: "checkmark.circle.fill").foregroundStyle(.green)
                    Button("Done", action: onDone)
                }
            } else if let chosen {
                Section("Password for \(chosen.ssid)") {
                    SecureField("Wi-Fi password", text: $password).textContentType(.password)
                    Button(joining ? "Joining…" : "Join") { Task { await join(chosen.ssid) } }
                        .disabled(joining || (chosen.secured && password.count < 8))
                    Button("Choose another network") { self.chosen = nil; status = nil }.disabled(joining)
                }
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
                    Button("Use this network") { chosen = WifiNetwork(ssid: otherName, rssi: -100, secured: true) }
                        .disabled(otherName.isEmpty)
                }
                if let status { Section { Text(status) } }
                Section { Button("Skip for now", action: onDone) }
            }
        }
        .task { await scan() }
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
        defer { joining = false }
        status = "Sending to your key…"
        do {
            try await KeyActions.setWifi(key: key.peripheralID, ssid: ssid, password: password)
        } catch {
            status = error.localizedDescription
            return
        }
        password = ""
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
