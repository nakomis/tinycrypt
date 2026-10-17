// A sign-in waiting for approval, opened by tapping its notification (CRYPT-30): most people tap the
// notification rather than long-pressing it for the Approve action.
import SwiftUI
import UserNotifications

struct PendingApproval: Identifiable, Equatable {
    let id = UUID()
    let key: KnownKey
    let site: String               // the push's body: "Sign in to example.com?" or "Install tinycrypt 1.2.3?"
    let expectedRequest: Data
    let sentAt: Date?

    enum Refusal: LocalizedError {
        case unknownKey, noRequest, needsSetup(String)
        var errorDescription: String? {
            switch self {
            case .unknownKey: return "That request came from a key this phone doesn't know."
            case .noRequest: return "That notification isn't a request this phone can approve."
            case .needsSetup(let name): return "This phone needs adding to \(name) again before it can approve."
            }
        }
    }

    /// From a notification, or why not. A push names its key (tc.key); only a push that names none
    /// (an early firmware) falls back to the phone's only key. On iOS every request must carry its
    /// hash: the app makes no local test notifications that could approve without one.
    @MainActor
    static func make(_ notification: UNNotification, keys: KeysModel) -> Result<PendingApproval, Refusal> {
        let key: KnownKey?
        if let thing = Presence.keyThing(notification) {
            key = keys.key(thing: thing)
        } else {
            key = keys.keys.count == 1 ? keys.keys.first : nil
        }
        guard let key else { return .failure(.unknownKey) }
        guard !keys.needsSetup(key) else { return .failure(.needsSetup(key.name)) }
        guard let hash = Presence.requestHash(notification) else { return .failure(.noRequest) }
        let tc = notification.request.content.userInfo["tc"] as? [String: Any]
        let sentAt = ((tc?["sentAt"] as? NSNumber)?.doubleValue).map { Date(timeIntervalSince1970: $0 / 1000) }
        return .success(PendingApproval(key: key, site: notification.request.content.body, expectedRequest: hash,
                                        sentAt: sentAt))
    }

    /// The key gives up on a request after about 30 s; a push can also turn up late. A push without
    /// sentAt (sent before the key's clock was set) relies on the key's own expiry.
    func stale(at now: Date = .now) -> Bool {
        sentAt.map { now.timeIntervalSince($0) * 1000 > Double(Presence.maxAgeMs) } ?? false
    }
    var stale: Bool { stale() }
}

struct ApproveView: View {
    let pending: PendingApproval
    let onClose: () -> Void
    @State private var state = Stage.asking

    enum Stage: Equatable { case asking, approving, approved, failed(String) }

    var body: some View {
        // Redrawn every second, so a request left open turns into "expired" by itself.
        TimelineView(.periodic(from: .now, by: 1)) { context in
            content(expired: state == .asking && pending.stale(at: context.date))
        }
    }

    @ViewBuilder
    private func content(expired: Bool) -> some View {
        VStack(spacing: 24) {
            Spacer()
            Image(systemName: "key.horizontal.fill").font(.system(size: 48)).foregroundStyle(.tint)
            Text(pending.site).font(.title2.bold()).multilineTextAlignment(.center)
            Text(pending.key.name).foregroundStyle(.secondary)
            Spacer()
            if state == .asking && !expired {
                // Between the request and the buttons, framed, so it's read before Approve is tapped.
                Text("Tip: next time, press and hold the notification and tap Approve to stay in the app you were using.")
                    .font(.callout.bold())
                    .multilineTextAlignment(.center)
                    .padding()
                    .overlay(RoundedRectangle(cornerRadius: 12).stroke(Color.accentColor, lineWidth: 2))
                Spacer()
            }
            switch state {
            case .asking:
                if expired {
                    Text("This request has expired. Try signing in again.").multilineTextAlignment(.center)
                    Button("Close", action: onClose).buttonStyle(.borderedProminent).controlSize(.large)
                } else {
                    Button { Task { await approve() } } label: {
                        Text("Approve").frame(maxWidth: .infinity)
                    }
                    .buttonStyle(.borderedProminent)
                    .controlSize(.large)
                    Button("Deny", role: .destructive, action: onClose).buttonStyle(.borderless)
                }
            case .approving:
                ProgressView("Approving…")
            case .approved:
                Label("Approved", systemImage: "checkmark.circle.fill").font(.title3).foregroundStyle(.green)
                    .task {
                        try? await Task.sleep(for: .seconds(1.5))
                        onClose()
                    }
            case .failed(let why):
                Text(why).multilineTextAlignment(.center)
                Text("You can still press the button on the key.").foregroundStyle(.secondary)
                Button("Close", action: onClose).buttonStyle(.borderedProminent).controlSize(.large)
            }
        }
        .padding()
    }

    private func approve() async {
        guard !pending.stale else { return }  // the screen switches to "expired" on its own
        state = .approving
        do {
            try await KeyActions.approve(key: pending.key.peripheralID, expectedRequest: pending.expectedRequest)
            state = .approved
        } catch {
            state = .failed(error.localizedDescription)
        }
    }
}
