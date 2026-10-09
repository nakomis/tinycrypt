// Spike app (CRYPT-11): a local notification stands in for the APNs push.
// Tapping Approve runs the BLE exchange from the notification action handler,
// whether the app is in the foreground, in the background or not running.
import SwiftUI
import UserNotifications
import WatchKit

/// Milliseconds since the kernel started this process. Near-zero-plus-launch-time at the tap means
/// a cold launch. (A lazy Swift global would be initialised inside the handler, so always read ~0.)
func processAgeMs() -> Int {
    var info = kinfo_proc()
    var size = MemoryLayout<kinfo_proc>.size
    var mib: [Int32] = [CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()]
    guard sysctl(&mib, 4, &info, &size, nil, 0) == 0 else { return -1 }
    let start = info.kp_proc.p_un.__p_starttime
    let started = Double(start.tv_sec) + Double(start.tv_usec) / 1_000_000
    return Int((Date().timeIntervalSince1970 - started) * 1000)
}

@main
struct TinycryptWatchApp: App {
    @WKApplicationDelegateAdaptor private var delegate: AppDelegate

    var body: some Scene {
        WindowGroup { ContentView().environmentObject(AppModel.shared) }
    }
}

final class AppDelegate: NSObject, WKApplicationDelegate, UNUserNotificationCenterDelegate {
    func applicationDidFinishLaunching() {
        // Must be set before launch completes, or a background launch for an action is missed.
        let center = UNUserNotificationCenter.current()
        center.delegate = self
        center.setNotificationCategories([Presence.category])
        // CRYPT-12: the real request arrives as an APNs push straight to the watch.
        WKApplication.shared().registerForRemoteNotifications()
    }

    func didRegisterForRemoteNotifications(withDeviceToken deviceToken: Data) {
        let token = deviceToken.map { String(format: "%02x", $0) }.joined()
        let model = AppModel.shared
        Task { @MainActor in
            if UserDefaults.standard.string(forKey: AppModel.pushTokenKey) != token {
                UserDefaults.standard.set(token, forKey: AppModel.pushTokenKey)
                model.record("push token \(token)")
            }
        }
    }

    func didFailToRegisterForRemoteNotificationsWithError(_ error: Error) {
        Task { @MainActor in AppModel.shared.record("push registration failed: \(error)") }
    }

    func userNotificationCenter(_ center: UNUserNotificationCenter, willPresent notification: UNNotification)
        async -> UNNotificationPresentationOptions {
        if let latency = Presence.pushLatency(notification) {
            await MainActor.run { AppModel.shared.record("push shown in foreground \(latency)") }
        }
        return [.banner, .sound]
    }

    @MainActor
    func userNotificationCenter(_ center: UNUserNotificationCenter, didReceive response: UNNotificationResponse) async {
        let tap = ContinuousClock.now
        let state = WKApplication.shared().applicationState
        let processAge = processAgeMs()
        let device = WKInterfaceDevice.current()
        let context = [
            "os": "\(device.systemName) \(device.systemVersion)",
            "model": modelIdentifier(),
            "action": response.actionIdentifier,
            "appState": ["active", "inactive", "background"][min(state.rawValue, 2)],
            "processAgeMs": String(processAge),
        ].merging(Presence.pushLatency(response.notification) ?? [:]) { a, _ in a }
        // SNS can't give APNs an expiry, so a push for an offline watch can turn up late. Never act on
        // a stale request; the key's challenge will have expired anyway. (sentAt is the key's clock.)
        if let age = context["sentToNowMs"].flatMap(Int.init), age > Presence.maxAgeMs {
            AppModel.shared.record("STALE request refused (\(age / 1000) s old) \(context)")
            return
        }
        guard response.actionIdentifier == Presence.approve else {
            AppModel.shared.record("\(response.actionIdentifier) (no BLE) \(context)")
            return
        }
        let route = AppModel.shared.route
        let report = await PresenceClient(mode: .approve, route: route, start: tap, context: context).run()
        AppModel.shared.record(report)
    }
}

/// Hardware model, e.g. "Watch7,5".
func modelIdentifier() -> String {
    var info = utsname()
    uname(&info)
    return withUnsafeBytes(of: &info.machine) { String(decoding: $0.prefix(while: { $0 != 0 }), as: UTF8.self) }
}

enum Presence {
    static let categoryID = "PRESENCE"
    static let approve = "APPROVE"
    static let deny = "DENY"
    /// Requests older than this (by the key's clock) are refused, not approved.
    static let maxAgeMs = 60_000

    /// Approve has no .foreground option: the point is to do the BLE work without opening the app.
    static let category = UNNotificationCategory(
        identifier: categoryID,
        actions: [
            UNNotificationAction(identifier: approve, title: "Approve", options: []),
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
        var out = [
            "pushId": tc["id"] as? String ?? "?",
            "sentToNowMs": String(ms(now) - sentAt),
            "sentToApnsDateMs": String(ms(notification.date) - sentAt),
        ]
        return out
    }

    static func schedule(after seconds: TimeInterval) async throws {
        let center = UNUserNotificationCenter.current()
        _ = try await center.requestAuthorization(options: [.alert, .sound])
        let content = UNMutableNotificationContent()
        content.title = "Sign in?"
        content.body = "tinycrypt spike: approve the test challenge"
        content.categoryIdentifier = categoryID
        content.sound = .default
        let trigger = UNTimeIntervalNotificationTrigger(timeInterval: seconds, repeats: false)
        try await center.add(UNNotificationRequest(identifier: UUID().uuidString, content: content, trigger: trigger))
    }
}

@MainActor
final class AppModel: ObservableObject {
    static let shared = AppModel()
    private static let logKey = "runLog"
    static let pushTokenKey = "apnsDeviceToken"

    @Published var log: [String] = UserDefaults.standard.stringArray(forKey: logKey) ?? []
    private static let routeKey = "route"
    // Persisted so a cold launch from the notification uses the route chosen in the UI.
    @Published var route = PresenceClient.Route(rawValue: UserDefaults.standard.string(forKey: routeKey) ?? "") ?? .retrieve {
        didSet { UserDefaults.standard.set(route.rawValue, forKey: Self.routeKey) }
    }

    func record(_ report: PresenceReport) {
        let head = report.error == nil ? "OK" : "FAIL"
        record(([ "\(head) \(report.mode) via \(report.route) \(report.context)" ] + report.steps).joined(separator: "\n"))
    }

    func record(_ line: String) {
        log.insert("\(Date.now.formatted(date: .omitted, time: .standard)) \(line)", at: 0)
        log = Array(log.prefix(20))
        UserDefaults.standard.set(log, forKey: Self.logKey)
    }
}
