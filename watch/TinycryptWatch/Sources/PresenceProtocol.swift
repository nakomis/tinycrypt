// GATT protocol v1 for watch presence approval (CRYPT-11; v1 adds the request hash, CRYPT-18).
// The key is the peripheral and the watch the central: watchOS has no
// CBPeripheralManager. Copied by hand into watch/tools/key-standin; keep them in step.
import CoreBluetooth

enum PresenceProtocol {
    static let service = CBUUID(string: "30CCC4F1-350D-4FDF-89C5-6120E2CC5044")
    /// read: domainTag ‖ nonce(32) ‖ request hash(32)
    static let challenge = CBUUID(string: "1C88EB1A-4D1E-4E79-AFC7-637C28E91503")
    /// write: r‖s (64) over the challenge bytes, CryptoKit signature(for:), which signs SHA-256
    static let response = CBUUID(string: "78DC86B2-6F57-478E-979C-22ED6E135A98")
    /// write: X‖Y (64); accepted only while the key is in enrol mode
    static let enrol = CBUUID(string: "A40698D1-315E-496D-8855-05283E3369A0")
    /// write: spike-only timing JSON
    static let telemetry = CBUUID(string: "C88C0AFB-1519-4B4F-9CA1-E5F9EEFFD259")

    static let domainTag = Data("tinycrypt-presence-v1".utf8)
    static let nonceLength = 32
    /// SHA-256 of the CTAP request the key is serving; the push carries the same hash as `tc.req`.
    static let requestHashLength = 32
    static var challengeLength: Int { domainTag.count + nonceLength + requestHashLength }
}
