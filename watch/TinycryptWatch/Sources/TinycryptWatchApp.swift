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
    }

    func userNotificationCenter(_ center: UNUserNotificationCenter, willPresent notification: UNNotification)
        async -> UNNotificationPresentationOptions {
        [.banner, .sound]
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
        ]
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

    /// Approve has no .foreground option: the point is to do the BLE work without opening the app.
    static let category = UNNotificationCategory(
        identifier: categoryID,
        actions: [
            UNNotificationAction(identifier: approve, title: "Approve", options: []),
            UNNotificationAction(identifier: deny, title: "Deny", options: [.destructive]),
        ],
        intentIdentifiers: [])

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
