import SwiftUI

struct ContentView: View {
    @EnvironmentObject private var model: AppModel
    @AppStorage(PresenceClient.peripheralIDKey) private var keyID: String?
    @State private var busy = false
    @State private var status: String?

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 10) {
                    Image(systemName: "key.horizontal.fill").font(.title2).foregroundStyle(.tint)
                    Text(keyID == nil ? "No key yet" : "Key added").font(.headline)
                    Text(keyID == nil
                         ? "Hold your key's button for 5 seconds, until it pulses magenta, then tap Add key."
                         : "Approve sign-ins from the notification, or press the button on the key.")
                        .font(.footnote)
                        .multilineTextAlignment(.center)
                        .foregroundStyle(.secondary)
                    Button(busy ? "Adding…" : keyID == nil ? "Add key" : "Add key again") { enrol() }
                        .disabled(busy)
                    if let status { Text(status).font(.footnote).multilineTextAlignment(.center) }
                    NavigationLink("Details") { DetailsView() }
                        .font(.footnote)
                }
            }
            .navigationTitle("tinycrypt")
        }
    }

    private func enrol() {
        busy = true
        status = nil
        Task {
            let report = await PresenceClient(mode: .enrol, route: .scan, start: .now, context: [:]).run()
            model.record(report)
            if report.error != nil {
                status = "Couldn't add the key. Is it plugged in and pulsing magenta?"
            } else {
                status = "Key added."
                // CRYPT-30: sign this watch up for the key's own pushes, through the key.
                if let id = keyID.flatMap(UUID.init), let token = UserDefaults.standard.string(forKey: AppModel.pushTokenKey) {
                    do {
                        try await KeyActions.register(key: id, as: .watch, token: token)
                        model.record("registered for this key's pushes")
                    } catch {
                        status = "Key added, but it couldn't sign this watch up for notifications. Add it again later."
                        model.record("push registration failed: \(error.localizedDescription)")
                    }
                }
            }
            busy = false
        }
    }
}

/// The spike's diagnostics (CRYPT-11), out of the way: this watch's key, the BLE route, a test
/// notification and the run log.
private struct DetailsView: View {
    @EnvironmentObject private var model: AppModel
    @State private var fingerprint = "…"

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 8) {
                Text("This watch's key \(fingerprint)").font(.caption.monospaced())
                Button("Test notification in 10 s") { arm() }
                // A wheel Picker is clipped unreadably on the watch; a toggle button is clearer.
                Button("Route: \(model.route == .retrieve ? "Retrieve" : "Scan")") {
                    model.route = model.route == .retrieve ? .scan : .retrieve
                }
                ForEach(model.log, id: \.self) { Text($0).font(.system(size: 11).monospaced()) }
            }
        }
        .navigationTitle("Details")
        .task { fingerprint = (try? KeyStore.fingerprint(KeyStore.key())) ?? "no Secure Enclave" }
    }

    private func arm() {
        Task {
            do {
                try await Presence.schedule(after: 10)
                model.record("armed: notification in 10 s")
            } catch {
                model.record("arm failed: \(error)")
            }
        }
    }
}
