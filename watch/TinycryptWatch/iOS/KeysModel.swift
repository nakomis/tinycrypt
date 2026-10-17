// The keys this phone is enrolled with (CRYPT-30), and its APNs token.
import Foundation
import UserNotifications

struct KnownKey: Codable, Identifiable, Hashable {
    var thing: String              // e.g. tinycrypt-prod-key-1; pushes name it as tc.key
    var peripheralID: UUID         // CoreBluetooth's identifier for it on this phone
    var registeredToken: String?   // the token the key last accepted for our pushes
    var fingerprint: String?       // of the Secure Enclave key it enrolled (nil: saved by an early build)
    var id: String { thing }
    var name: String { thing.split(separator: "-").suffix(2).joined(separator: "-") }
}

@MainActor
final class KeysModel: ObservableObject {
    static let shared = KeysModel()
    private static let keysKey = "knownKeys"
    private static let tokenKey = "apnsDeviceToken"

    @Published private(set) var keys: [KnownKey] = {
        guard let data = UserDefaults.standard.data(forKey: keysKey) else { return [] }
        return (try? JSONDecoder().decode([KnownKey].self, from: data)) ?? []
    }()
    @Published private(set) var token: String? = UserDefaults.standard.string(forKey: tokenKey)
    /// A sign-in whose notification was tapped, shown full screen until approved, denied or closed.
    @Published var pending: PendingApproval?
    /// Something the user should know that has no screen of its own (e.g. a push from an unknown key).
    @Published var notice: String?
    @Published private(set) var notificationsAllowed = true

    /// This phone's Secure Enclave key, if it has one (a phone restored from a backup doesn't).
    let fingerprint: String? = KeyStore.existingKey().map(KeyStore.fingerprint)

    /// Screenshots for the App Store (debug builds, TC_SCREENSHOT set): made-up keys, nothing saved.
    private(set) var demo = false

    func loadDemo() {
        demo = true
        keys = [KnownKey(thing: "tinycrypt-prod-key-1", peripheralID: UUID(), registeredToken: "demo", fingerprint: nil)]
    }

    /// Enrolled with a key this phone no longer has: it can't approve until it's added again.
    func needsSetup(_ key: KnownKey) -> Bool {
        if demo { return false }
        guard let enrolled = key.fingerprint else { return fingerprint == nil }
        return enrolled != fingerprint
    }

    func key(thing: String) -> KnownKey? { keys.first { $0.thing == thing } }

    func save(_ key: KnownKey) {
        keys.removeAll { $0.thing == key.thing || $0.peripheralID == key.peripheralID }
        keys.append(key)
        keys.sort { $0.thing < $1.thing }
        persist()
    }

    func forget(_ key: KnownKey) {
        keys.removeAll { $0.thing == key.thing }
        persist()
    }

    func refreshNotificationPermission() async {
        let status = await UNUserNotificationCenter.current().notificationSettings().authorizationStatus
        notificationsAllowed = status == .authorized || status == .provisional || status == .notDetermined
    }

    /// A new token (first launch, or after a restore) has to reach every key again, over BLE, so this
    /// tries each key that has an older one; a key out of range is retried on the next launch.
    func tokenArrived(_ token: String) {
        if self.token != token {
            self.token = token
            UserDefaults.standard.set(token, forKey: Self.tokenKey)
        }
        Task { await reregister() }
    }

    private var reregistering = false

    func reregister() async {
        guard let token, !reregistering else { return }
        reregistering = true
        defer { reregistering = false }
        for key in keys where key.registeredToken != token && !needsSetup(key) {
            do {
                try await KeyActions.register(key: key.peripheralID, as: .phone, token: token)
                var updated = key
                updated.registeredToken = token
                save(updated)
            } catch {
                // Out of range or unplugged: next time.
            }
        }
    }

    private func persist() {
        guard !demo else { return }
        UserDefaults.standard.set(try? JSONEncoder().encode(keys), forKey: Self.keysKey)
    }
}
