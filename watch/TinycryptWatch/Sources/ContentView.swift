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
