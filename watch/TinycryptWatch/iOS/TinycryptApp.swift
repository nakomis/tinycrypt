// tinycrypt for iPhone and iPad (CRYPT-30): adds a key (enrol, Wi-Fi), and approves its sign-ins from
// the notification, like the watch app, over BLE with this device's Secure Enclave key.
import SwiftUI
import UIKit
import UserNotifications

@main
struct TinycryptApp: App {
    @UIApplicationDelegateAdaptor private var delegate: AppDelegate

    var body: some Scene {
        WindowGroup { KeysView().environmentObject(KeysModel.shared) }
    }
}

final class AppDelegate: NSObject, UIApplicationDelegate, UNUserNotificationCenterDelegate {
    func application(_ application: UIApplication,
                     didFinishLaunchingWithOptions options: [UIApplication.LaunchOptionsKey: Any]? = nil) -> Bool {
        // Must be set before launch completes, or a background launch for an action is missed.
        let center = UNUserNotificationCenter.current()
        center.delegate = self
        center.setNotificationCategories([Presence.category])
        // A token whatever the permission: the key registers it, and the app shows a banner if
        // notifications are off (permission itself is asked for while adding a key).
        application.registerForRemoteNotifications()
        return true
    }

    func application(_ application: UIApplication, didRegisterForRemoteNotificationsWithDeviceToken token: Data) {
        Task { @MainActor in KeysModel.shared.tokenArrived(token.hex) }
    }

    func userNotificationCenter(_ center: UNUserNotificationCenter, willPresent notification: UNNotification)
        async -> UNNotificationPresentationOptions {
        [.banner, .sound]
    }

    @MainActor
    func userNotificationCenter(_ center: UNUserNotificationCenter, didReceive response: UNNotificationResponse) async {
        let notification = response.notification
        let model = KeysModel.shared
        switch response.actionIdentifier {
        case UNNotificationDefaultActionIdentifier:
            // A plain tap opens the app on the request, with its own Approve button.
            switch PendingApproval.make(notification, keys: model) {
            case .success(let pending): model.pending = pending
            case .failure(let why): model.notice = why.localizedDescription
            }
        case Presence.approve:
            switch PendingApproval.make(notification, keys: model) {
            case .failure(let why):
                await Self.tellUser(why.localizedDescription)
            case .success(let pending):
                if pending.stale { return await Self.tellUser(KeyActions.ApproveError.expired.localizedDescription) }
                do {
                    try await KeyActions.approve(key: pending.key.peripheralID, expectedRequest: pending.expectedRequest)
                } catch {
                    await Self.tellUser("\(error.localizedDescription) You can still press the button on the key.")
                }
            }
        default:
            break  // Deny, or dismissed: the key times out and the sign-in fails
        }
    }

    /// An approval from the notification runs with the app in the background, so a failure is said
    /// with a notification of its own (no category: nothing to act on).
    static func tellUser(_ text: String) async {
        let content = UNMutableNotificationContent()
        content.title = "Not approved"
        content.body = text
        try? await UNUserNotificationCenter.current()
            .add(UNNotificationRequest(identifier: UUID().uuidString, content: content, trigger: nil))
    }
}
