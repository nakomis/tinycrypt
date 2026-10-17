// GATT protocol for presence approval (CRYPT-11; v1 adds the request hash, CRYPT-18; CRYPT-30 adds a
// phone approver and setup). The key is the peripheral and the watch or phone the central: watchOS
// has no CBPeripheralManager. Copied by hand into watch/tools/key-standin; keep them in step. The key's
// side: embedded/esp32s3/main/ble_presence.h and embedded/port/setup_msg.h.
import CoreBluetooth

enum PresenceProtocol {
    static let service = CBUUID(string: "30CCC4F1-350D-4FDF-89C5-6120E2CC5044")
    /// read: domainTag ‖ nonce(32) ‖ request hash(32)
    static let challenge = CBUUID(string: "1C88EB1A-4D1E-4E79-AFC7-637C28E91503")
    /// write: r‖s (64) over the challenge bytes, CryptoKit signature(for:), which signs SHA-256
    static let response = CBUUID(string: "78DC86B2-6F57-478E-979C-22ED6E135A98")
    /// write: kind ‖ X‖Y (65), or X‖Y (64) for a watch; accepted only while the key is in enrol mode
    static let enrol = CBUUID(string: "A40698D1-315E-496D-8855-05283E3369A0")
    /// write: spike-only timing JSON
    static let telemetry = CBUUID(string: "C88C0AFB-1519-4B4F-9CA1-E5F9EEFFD259")
    /// read: {"thing", "version", "wifi": {"ssid", "state"}, "iot", "watch", "phone"} (enrolled devices)
    static let info = CBUUID(string: "8CB61CE4-75BA-4862-8079-5B71FD663294")
    /// write {"ssid", "password"} or {"scan": true}; read the last scan: [["ssid", rssi, secured], ...]
    static let wifi = CBUUID(string: "BCF39609-8B45-427E-9BFC-8EB1456BAB20")
    /// write: {"kind": "watch" | "phone", "token": "<APNs device token, hex>"}
    static let register = CBUUID(string: "3F07D86E-1CAC-4EA1-9FD5-AD819760957A")

    static let domainTag = Data("tinycrypt-presence-v1".utf8)
    static let nonceLength = 32
    /// SHA-256 of the CTAP request the key is serving; the push carries the same hash as `tc.req`.
    static let requestHashLength = 32
    static var challengeLength: Int { domainTag.count + nonceLength + requestHashLength }

    /// Which approver slot a device enrols into: a key has one of each.
    enum Kind: UInt8 {
        case watch = 1, phone = 2
        var name: String { self == .watch ? "watch" : "phone" }
    }

    /// The challenge is well formed and, for a push from the key, carries the request hash the push
    /// named. Only the app's own local test notification passes nil.
    static func isSignable(_ challenge: Data, expectedRequest: Data?) -> Bool {
        guard challenge.count == challengeLength, challenge.starts(with: domainTag) else { return false }
        if let expectedRequest { return challenge.suffix(requestHashLength) == expectedRequest }
        return true
    }
}

/// What the key's `info` characteristic says.
struct KeyInfo: Codable, Equatable {
    struct Wifi: Codable, Equatable {
        var ssid: String
        var state: String  // unset | connecting | connected | wrong-password | not-found | failed
    }
    var thing: String
    var version: String
    var wifi: Wifi
    var iot: Bool
    var watch: Bool
    var phone: Bool

    /// "tinycrypt-prod-key-1" -> "key-1"
    var shortName: String { thing.split(separator: "-").suffix(2).joined(separator: "-") }
}

/// One network from the key's Wi-Fi scan.
struct WifiNetwork: Identifiable, Hashable {
    var ssid: String
    var rssi: Int
    var secured: Bool
    var id: String { ssid }

    /// The key's scan read: [["ssid", rssi, secured 0|1], ...]
    static func parse(_ data: Data) -> [WifiNetwork] {
        guard let rows = try? JSONSerialization.jsonObject(with: data) as? [[Any]] else { return [] }
        return rows.compactMap { row in
            guard row.count == 3, let ssid = row[0] as? String, let rssi = row[1] as? Int,
                  let secured = row[2] as? Int else { return nil }
            return WifiNetwork(ssid: ssid, rssi: rssi, secured: secured != 0)
        }
    }
}
