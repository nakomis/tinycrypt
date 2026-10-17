// The presence notification (CRYPT-11, CRYPT-12), shared by the watch and iPhone apps (CRYPT-30).
import Foundation
import UserNotifications

enum Presence {
    static let categoryID = "PRESENCE"
    static let approve = "APPROVE"
    static let deny = "DENY"
    /// Requests older than this (by the key's clock) are refused, not approved.
    static let maxAgeMs = 60_000

    /// Approve has no .foreground option: the point is to do the BLE work without opening the app.
    /// On a phone (CRYPT-30) it needs Face ID or the passcode first: a locked phone can be picked up
    /// by anyone, a watch only counts while it's on your wrist and unlocked.
    #if os(iOS)
    static let approveOptions: UNNotificationActionOptions = [.authenticationRequired]
    #else
    static let approveOptions: UNNotificationActionOptions = []
    #endif
    static let category = UNNotificationCategory(
        identifier: categoryID,
        actions: [
            UNNotificationAction(identifier: approve, title: "Approve", options: approveOptions),
            UNNotificationAction(identifier: deny, title: "Deny", options: [.destructive]),
        ],
        intentIdentifiers: [])

    /// For an APNs push from the relay (CRYPT-12): milliseconds from the publisher's sentAt to `now` on
    /// the watch (arrival, in willPresent; the tap, in didReceive). `notification.date` is APNs' own
    /// stamp, not the watch's arrival, so it's reported separately. The clocks are the Mac's, AWS's and
    /// the watch's, all NTP-synced, so expect tens of ms of skew.
    static func pushLatency(_ notification: UNNotification, now: Date = .now) -> [String: String]? {
        guard let tc = notification.request.content.userInfo["tc"] as? [String: Any],
              let sentAt = (tc["sentAt"] as? NSNumber)?.int64Value else { return nil }
        let ms = { (d: Date) in Int64(d.timeIntervalSince1970 * 1000) }
        let out = [
            "pushId": tc["id"] as? String ?? "?",
            "sentToNowMs": String(ms(now) - sentAt),
            "sentToApnsDateMs": String(ms(notification.date) - sentAt),
        ]
        return out
    }

    /// The request hash a push from the key carries (tc.req: 64 lower-case hex digits), or nil if it
    /// is missing or malformed. All zeros is refused too: the key never binds a wait to that.
    static func requestHash(_ notification: UNNotification) -> Data? {
        guard let tc = notification.request.content.userInfo["tc"] as? [String: Any],
              let hex = tc["req"] as? String else { return nil }
        let digits = Array(hex.utf8)
        guard digits.count == 2 * PresenceProtocol.requestHashLength else { return nil }
        func nibble(_ c: UInt8) -> UInt8? {
            switch c {
            case UInt8(ascii: "0")...UInt8(ascii: "9"): return c - UInt8(ascii: "0")
            case UInt8(ascii: "a")...UInt8(ascii: "f"): return c - UInt8(ascii: "a") + 10
            default: return nil
            }
        }
        var bytes = Data(capacity: PresenceProtocol.requestHashLength)
        for i in stride(from: 0, to: digits.count, by: 2) {
            guard let hi = nibble(digits[i]), let lo = nibble(digits[i + 1]) else { return nil }
            bytes.append(hi << 4 | lo)
        }
        return bytes.contains(where: { $0 != 0 }) ? bytes : nil
    }

    static func schedule(after seconds: TimeInterval) async throws {
        let center = UNUserNotificationCenter.current()
        _ = try await center.requestAuthorization(options: [.alert, .sound])
        let content = UNMutableNotificationContent()
        content.title = "Sign in?"
        content.body = "Test: approve the key's waiting request"
        content.categoryIdentifier = categoryID
        content.sound = .default
        let trigger = UNTimeIntervalNotificationTrigger(timeInterval: seconds, repeats: false)
        try await center.add(UNNotificationRequest(identifier: UUID().uuidString, content: content, trigger: trigger))
    }

    /// The Thing name of the key a push came from (tc.key), e.g. "tinycrypt-prod-key-1".
    static func keyThing(_ notification: UNNotification) -> String? {
        (notification.request.content.userInfo["tc"] as? [String: Any])?["key"] as? String
    }
}
