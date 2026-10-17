// What the apps do with a key (CRYPT-30), each one connection: enrol, approve, and setup.
import CryptoKit
import Foundation

@MainActor
enum KeyActions {
    /// Enrols this device into its slot on the closest key in enrol mode. Returns the key's info, its
    /// peripheral identifier and the fingerprint of the Secure Enclave key it now trusts. Registering
    /// for pushes is a separate step, so a failure there doesn't lose the enrolment.
    static func enrol(as kind: PresenceProtocol.Kind) async throws -> (KeyInfo, UUID, String) {
        let link = KeyLink()
        defer { link.close() }
        try await link.openForEnrolment()
        let key = try KeyStore.key()
        try await link.write(PresenceProtocol.enrol, Data([kind.rawValue]) + KeyStore.publicKeyXY(key))
        let info = try await link.readJSON(PresenceProtocol.info, as: KeyInfo.self)
        return (info, link.peripheral!.identifier, KeyStore.fingerprint(key))
    }

    enum ApproveError: LocalizedError {
        case expired, noKey, wrongRequest
        var errorDescription: String? {
            switch self {
            case .expired: return "This request has expired. Try signing in again."
            case .noKey: return "This \(DeviceName.current) needs adding to the key again."
            case .wrongRequest: return "The key is asking about a different sign-in, so nothing was approved."
            }
        }
    }

    /// Reads the key's challenge, checks it is for the request that was pushed, signs and answers, all
    /// within `deadline`: the key gives up after about 30 s, and iOS gives a background action little time.
    static func approve(key: UUID, expectedRequest: Data?, deadline: Duration = .seconds(25)) async throws {
        try await withThrowingTaskGroup(of: Void.self) { group in
            group.addTask { try await approveNow(key: key, expectedRequest: expectedRequest) }
            group.addTask {
                try await Task.sleep(for: deadline)
                throw KeyLinkError.timedOut("approval")
            }
            defer { group.cancelAll() }
            try await group.next()
        }
    }

    private static func approveNow(key: UUID, expectedRequest: Data?) async throws {
        // Never make a key here: one the key hasn't enrolled can't approve anything.
        guard let signer = KeyStore.existingKey() else { throw ApproveError.noKey }
        let link = KeyLink()
        defer { link.close() }
        try await link.open(peripheralID: key)
        let challenge = try await link.read(PresenceProtocol.challenge)
        guard PresenceProtocol.isSignable(challenge, expectedRequest: expectedRequest) else {
            throw ApproveError.wrongRequest
        }
        try await link.write(PresenceProtocol.response, try signer.signature(for: challenge).rawRepresentation)
    }

    static func info(key: UUID) async throws -> KeyInfo {
        let link = KeyLink()
        defer { link.close() }
        try await link.open(peripheralID: key)
        return try await link.readJSON(PresenceProtocol.info, as: KeyInfo.self)
    }

    /// Asks the key to scan (it takes a few seconds), then reads what it heard.
    static func scanWifi(key: UUID) async throws -> [WifiNetwork] {
        let link = KeyLink()
        defer { link.close() }
        try await link.open(peripheralID: key)
        try await link.writeJSON(PresenceProtocol.wifi, ["scan": true])
        try await Task.sleep(for: .seconds(4))
        return WifiNetwork.parse(try await link.read(PresenceProtocol.wifi))
    }

    static func setWifi(key: UUID, ssid: String, password: String) async throws {
        let link = KeyLink()
        defer { link.close() }
        try await link.open(peripheralID: key)
        try await link.writeJSON(PresenceProtocol.wifi, ["ssid": ssid, "password": password])
    }

    static func register(key: UUID, as kind: PresenceProtocol.Kind, token: String) async throws {
        let link = KeyLink()
        defer { link.close() }
        try await link.open(peripheralID: key)
        try await link.writeJSON(PresenceProtocol.register, ["kind": kind.name, "token": token])
    }
}

/// Hex for an APNs device token.
extension Data {
    var hex: String { map { String(format: "%02x", $0) }.joined() }
}
