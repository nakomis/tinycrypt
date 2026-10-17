// A sign-in waiting for approval, opened by tapping its notification (CRYPT-30): most people tap the
// notification rather than long-pressing it for the Approve action.
import SwiftUI
import UserNotifications

struct PendingApproval: Identifiable, Equatable {
    let id = UUID()
    let key: KnownKey
    let site: String               // the push's body: "Sign in to example.com?" or "Install tinycrypt 1.2.3?"
    let expectedRequest: Data?     // nil only for the app's own test notification
    let sentAt: Date?

    /// From a tapped notification, or nil if it isn't one we can act on.
    @MainActor
    init?(_ notification: UNNotification, keys: KeysModel) {
        let all = keys.keys
        guard let key = Presence.keyThing(notification).flatMap(keys.key(thing:)) ?? (all.count == 1 ? all.first : nil)
        else { return nil }
        var expected: Data?
        if notification.request.trigger is UNPushNotificationTrigger {
            guard let hash = Presence.requestHash(notification) else { return nil }
            expected = hash
        }
        let tc = notification.request.content.userInfo["tc"] as? [String: Any]
        self.key = key
        self.site = notification.request.content.body
        self.expectedRequest = expected
        self.sentAt = ((tc?["sentAt"] as? NSNumber)?.doubleValue).map { Date(timeIntervalSince1970: $0 / 1000) }
    }

    /// The key gives up on a request after about 30 s; a push can also turn up late.
    var stale: Bool { sentAt.map { Date.now.timeIntervalSince($0) * 1000 > Double(Presence.maxAgeMs) } ?? false }
}

struct ApproveView: View {
    let pending: PendingApproval
    let onClose: () -> Void
    @State private var state = Stage.asking

    enum Stage: Equatable { case asking, approving, approved, failed(String) }

    var body: some View {
        VStack(spacing: 24) {
            Spacer()
            Image(systemName: "key.horizontal.fill").font(.system(size: 48)).foregroundStyle(.tint)
            Text(pending.site).font(.title2.bold()).multilineTextAlignment(.center)
            Text(pending.key.name).foregroundStyle(.secondary)
            Spacer()
            if state == .asking && !pending.stale {
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
                if pending.stale {
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
        state = .approving
        do {
            try await KeyActions.approve(key: pending.key.peripheralID, expectedRequest: pending.expectedRequest)
            state = .approved
        } catch {
            state = .failed(error.localizedDescription)
        }
    }
}
