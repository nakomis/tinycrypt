// What the apps do with a key (CRYPT-30), each one connection: enrol, approve, and setup.
import CryptoKit
import Foundation

@MainActor
enum KeyActions {
    /// Enrols this device into its slot on a key in enrol mode (the closest one), then registers it for
    /// that key's pushes if we have a token yet. Returns the key's info and its peripheral identifier.
    static func enrol(as kind: PresenceProtocol.Kind, token: String?) async throws -> (KeyInfo, UUID) {
        let link = KeyLink()
        defer { link.close() }
        try await link.open(peripheralID: nil)
        let pub = KeyStore.publicKeyXY(try KeyStore.key())
        try await link.write(PresenceProtocol.enrol, Data([kind.rawValue]) + pub)
        let info = try await link.readJSON(PresenceProtocol.info, as: KeyInfo.self)
        if let token {
            try await link.writeJSON(PresenceProtocol.register, ["kind": kind.name, "token": token])
        }
        return (info, link.peripheral!.identifier)
    }

    /// Reads the key's challenge, checks it is for the request that was pushed, signs and answers.
    static func approve(key: UUID, expectedRequest: Data?) async throws {
        let link = KeyLink()
        defer { link.close() }
        try await link.open(peripheralID: key)
        let challenge = try await link.read(PresenceProtocol.challenge)
        guard PresenceProtocol.isSignable(challenge, expectedRequest: expectedRequest) else {
            throw KeyLinkError.refused("that challenge isn't for this request: not signed")
        }
        let sig = try KeyStore.key().signature(for: challenge).rawRepresentation
        try await link.write(PresenceProtocol.response, sig)
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
