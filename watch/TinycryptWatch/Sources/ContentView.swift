import SwiftUI

struct ContentView: View {
    @EnvironmentObject private var model: AppModel
    @State private var fingerprint = "…"
    @State private var busy = false

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 8) {
                Text("Key \(fingerprint)").font(.caption.monospaced())
                Button("Enrol with key") { enrol() }.disabled(busy)
                Button("Arm in 10 s") { arm() }.disabled(busy)
                // A wheel Picker is clipped unreadably on the watch; a toggle button is clearer.
                Button("Route: \(model.route == .retrieve ? "Retrieve" : "Scan")") {
                    model.route = model.route == .retrieve ? .scan : .retrieve
                }
                ForEach(model.log, id: \.self) { Text($0).font(.system(size: 11).monospaced()) }
            }
        }
        .task {
            fingerprint = (try? KeyStore.fingerprint(KeyStore.key())) ?? "no Secure Enclave"
        }
    }

    private func enrol() {
        busy = true
        Task {
            let report = await PresenceClient(mode: .enrol, route: .scan, start: .now, context: [:]).run()
            model.record(report)
            // CRYPT-30: sign this watch up for the key's own pushes, through the key.
            if report.error == nil,
               let id = UserDefaults.standard.string(forKey: PresenceClient.peripheralIDKey).flatMap(UUID.init),
               let token = UserDefaults.standard.string(forKey: AppModel.pushTokenKey) {
                do {
                    try await KeyActions.register(key: id, as: .watch, token: token)
                    model.record("registered for this key's pushes")
                } catch {
                    model.record("push registration failed: \(error.localizedDescription)")
                }
            }
            busy = false
        }
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
