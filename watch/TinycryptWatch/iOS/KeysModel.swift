// The keys this phone is enrolled with (CRYPT-30), and its APNs token.
import Foundation

struct KnownKey: Codable, Identifiable, Hashable {
    var thing: String          // e.g. tinycrypt-prod-key-1; pushes name it as tc.key
    var peripheralID: UUID     // CoreBluetooth's identifier for it on this phone
    var registeredToken: String?  // the token the key last accepted for our pushes
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

    /// A new token (first launch, or after a restore) has to reach every key again, over BLE, so this
    /// tries each key that has an older one; a key out of range is retried on the next launch.
    func tokenArrived(_ token: String) {
        if self.token != token {
            self.token = token
            UserDefaults.standard.set(token, forKey: Self.tokenKey)
        }
        Task { await reregister() }
    }

    func reregister() async {
        guard let token else { return }
        for key in keys where key.registeredToken != token {
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
        UserDefaults.standard.set(try? JSONEncoder().encode(keys), forKey: Self.keysKey)
    }
}
