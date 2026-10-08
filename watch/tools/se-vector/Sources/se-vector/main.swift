// Signs nonces with a throwaway Secure Enclave P-256 key, the same way the
// watch will (CryptoKit `signature(for: Data)`), and writes them as JSON test
// vectors for the key-side verifier.
//
//   swift run --package-path watch/tools/se-vector se-vector > embedded/tests/vectors/se_p256.json
//
// ECDSA is randomised: every run gives different signatures. Regenerate only
// on purpose. Refuses to run without a Secure Enclave rather than falling back
// to a software key, which would prove nothing.
import CryptoKit
import Foundation

func hex(_ data: some DataProtocol) -> String {
    data.map { String(format: "%02x", $0) }.joined()
}

func fail(_ message: String) -> Never {
    FileHandle.standardError.write(Data("se-vector: \(message)\n".utf8))
    exit(1)
}

guard SecureEnclave.isAvailable else {
    fail("no Secure Enclave on this machine; refusing to fall back to a software key")
}

let key: SecureEnclave.P256.Signing.PrivateKey
do {
    key = try SecureEnclave.P256.Signing.PrivateKey()
} catch {
    fail("could not create a Secure Enclave key: \(error)")
}

// x963 is 04 || X || Y; the wire and storage format is the bare X || Y.
let x963 = key.publicKey.x963Representation
guard x963.count == 65, x963.first == 0x04 else { fail("unexpected public key encoding") }
let pub = x963.dropFirst()

var random = Data(count: 32)
let status = random.withUnsafeMutableBytes { SecRandomCopyBytes(kSecRandomDefault, 32, $0.baseAddress!) }
guard status == errSecSuccess else { fail("SecRandomCopyBytes failed: \(status)") }

let messages: [(String, Data)] = [
    ("fixed nonce 00..1f", Data((0..<32).map { UInt8($0) })),
    ("random nonce", random),
]

var cases: [[String: String]] = []
for (name, message) in messages {
    let signature: P256.Signing.ECDSASignature
    do {
        signature = try key.signature(for: message)
    } catch {
        fail("signing failed: \(error)")
    }
    let digest = SHA256.hash(data: message)
    // The convention under test: signature(for: Data) signs SHA-256(message).
    guard key.publicKey.isValidSignature(signature, for: message),
          key.publicKey.isValidSignature(signature, for: digest) else {
        fail("CryptoKit rejected its own signature for '\(name)'")
    }
    cases.append([
        "name": name,
        "message": hex(message),
        "sha256": hex(Data(digest)),
        "sig": hex(signature.rawRepresentation),
    ])
}

let info = ProcessInfo.processInfo
let output: [String: Any] = [
    "description": "P-256 signatures from a real Secure Enclave key via CryptoKit signature(for: Data). "
        + "The signed value is SHA-256(message). Formats: pub = X||Y (64 bytes), sig = r||s (64 bytes).",
    "source": "secure-enclave",
    "platform": "macOS \(info.operatingSystemVersionString)",
    "generated": ISO8601DateFormatter().string(from: Date()),
    "pub": hex(pub),
    "cases": cases,
]
let json = try JSONSerialization.data(withJSONObject: output, options: [.prettyPrinted, .sortedKeys])
FileHandle.standardOutput.write(json)
FileHandle.standardOutput.write(Data("\n".utf8))
