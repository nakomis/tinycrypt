// The watch's presence key: a Secure Enclave P-256 key. Only its encrypted
// dataRepresentation (usable on this device's Secure Enclave alone) is kept in
// the Keychain, readable after first unlock so a background launch can sign.
import CryptoKit
import Foundation
import Security

enum KeyStore {
    private static let account = "presence-key"
    private static let service = "com.nakomis.tinycrypt.watch"

    static func key() throws -> SecureEnclave.P256.Signing.PrivateKey {
        if let blob = load() {
            return try SecureEnclave.P256.Signing.PrivateKey(dataRepresentation: blob)
        }
        let key = try SecureEnclave.P256.Signing.PrivateKey()
        try save(key.dataRepresentation)
        return key
    }

    /// X‖Y (64 bytes): x963Representation without the leading 0x04.
    static func publicKeyXY(_ key: SecureEnclave.P256.Signing.PrivateKey) -> Data {
        key.publicKey.x963Representation.dropFirst()
    }

    static func fingerprint(_ key: SecureEnclave.P256.Signing.PrivateKey) -> String {
        SHA256.hash(data: publicKeyXY(key)).prefix(4).map { String(format: "%02x", $0) }.joined()
    }

    private static var query: [String: Any] {
        [kSecClass as String: kSecClassGenericPassword,
         kSecAttrService as String: service,
         kSecAttrAccount as String: account]
    }

    private static func load() -> Data? {
        var q = query
        q[kSecReturnData as String] = true
        var out: AnyObject?
        guard SecItemCopyMatching(q as CFDictionary, &out) == errSecSuccess else { return nil }
        return out as? Data
    }

    private static func save(_ blob: Data) throws {
        var q = query
        q[kSecValueData as String] = blob
        q[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
        let status = SecItemAdd(q as CFDictionary, nil)
        guard status == errSecSuccess else {
            throw NSError(domain: NSOSStatusErrorDomain, code: Int(status))
        }
    }
}
