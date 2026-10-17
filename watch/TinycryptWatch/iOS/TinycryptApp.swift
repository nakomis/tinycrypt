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
        Task {
            _ = try? await center.requestAuthorization(options: [.alert, .sound])
            await MainActor.run { application.registerForRemoteNotifications() }
        }
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
        guard response.actionIdentifier == Presence.approve else { return }
        let notification = response.notification
        // SNS can't give APNs an expiry, so a push can turn up late: never act on a stale one.
        if let age = Presence.pushLatency(notification)?["sentToNowMs"].flatMap(Int.init), age > Presence.maxAgeMs {
            return
        }
        // A push from a key must name its request, and the challenge must match it. Only the app's
        // own local test notification has none.
        var expected: Data?
        if notification.request.trigger is UNPushNotificationTrigger {
            guard let hash = Presence.requestHash(notification) else { return }
            expected = hash
        }
        let keys = KeysModel.shared.keys
        guard let key = Presence.keyThing(notification).flatMap(KeysModel.shared.key(thing:)) ?? (keys.count == 1 ? keys.first : nil)
        else { return }
        try? await KeyActions.approve(key: key.peripheralID, expectedRequest: expected)
    }
}
