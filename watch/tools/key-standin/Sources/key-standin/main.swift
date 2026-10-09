// key-standin: plays the key's BLE peripheral role for CRYPT-11, so the watch
// app can be tested before the ESP32 has a BLE stack.
//
//   swift run --package-path watch/tools/key-standin key-standin [options]
//     --enrol               accept the watch's public key (written to the enrol characteristic)
//     --encrypt             require an encrypted (paired) link for every characteristic
//     --emit-vector PATH    after --count valid responses, write them as a test vector file
//     --count N             valid responses to collect for --emit-vector (default 2)
//     --sigcheck PATH       also verify with the C key-side verifier (tinycrypt-sigcheck)
//
// Every valid response re-arms a fresh nonce, so a signature is never accepted twice.
import CoreBluetooth
import CryptoKit
import Foundation

struct Options {
    var enrol = false
    var encrypt = false
    var vectorPath: String?
    var count = 2
    var sigcheck: String?

    init(_ args: [String]) {
        var it = args.makeIterator()
        while let arg = it.next() {
            switch arg {
            case "--enrol": enrol = true
            case "--encrypt": encrypt = true
            case "--emit-vector": vectorPath = it.next()
            case "--count": count = it.next().flatMap(Int.init) ?? count
            case "--sigcheck": sigcheck = it.next()
            default:
                FileHandle.standardError.write(Data("unknown option \(arg)\n".utf8))
                exit(2)
            }
        }
    }
}

func hex(_ data: Data) -> String { data.map { String(format: "%02x", $0) }.joined() }

func unhex(_ s: String) -> Data? {
    let s = s.trimmingCharacters(in: .whitespacesAndNewlines)
    guard s.count % 2 == 0 else { return nil }
    var out = Data()
    var i = s.startIndex
    while i < s.endIndex {
        let j = s.index(i, offsetBy: 2)
        guard let b = UInt8(s[i..<j], radix: 16) else { return nil }
        out.append(b)
        i = j
    }
    return out
}

func log(_ msg: String) {
    let t = Date.now.formatted(.dateTime.hour().minute().second().secondFraction(.fractional(3)))
    print("\(t) \(msg)")
    fflush(stdout)
}

let stateDir = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent(".tinycrypt-standin")
let pubFile = stateDir.appendingPathComponent("watch-pub.hex")

final class Standin: NSObject, CBPeripheralManagerDelegate {
    let opts: Options
    var manager: CBPeripheralManager!
    var pub: Data?
    var challenge = Data()
    var armedAt = ContinuousClock.now
    var cases: [[String: String]] = []
    var watchPlatform = "watchOS"
    var chars: [CBUUID: CBMutableCharacteristic] = [:]

    init(_ opts: Options) {
        self.opts = opts
        super.init()
        pub = (try? String(contentsOf: pubFile, encoding: .utf8)).flatMap(unhex)
        manager = CBPeripheralManager(delegate: self, queue: nil)
    }

    func arm() {
        var nonce = Data(count: PresenceProtocol.nonceLength)
        let rc = nonce.withUnsafeMutableBytes { SecRandomCopyBytes(kSecRandomDefault, PresenceProtocol.nonceLength, $0.baseAddress!) }
        precondition(rc == errSecSuccess, "SecRandomCopyBytes failed")
        challenge = PresenceProtocol.domainTag + nonce
        armedAt = .now
        log("armed nonce \(hex(nonce).prefix(16))…")
    }

    func ms(since t: ContinuousClock.Instant) -> Int {
        let d = ContinuousClock.now - t
        return Int(d.components.seconds * 1000 + d.components.attoseconds / 1_000_000_000_000_000)
    }

    // MARK: setup

    func peripheralManagerDidUpdateState(_ peripheral: CBPeripheralManager) {
        log("bluetooth state \(peripheral.state.rawValue)")
        guard peripheral.state == .poweredOn else { return }
        let read: CBAttributePermissions = opts.encrypt ? .readEncryptionRequired : .readable
        let write: CBAttributePermissions = opts.encrypt ? .writeEncryptionRequired : .writeable
        let service = CBMutableService(type: PresenceProtocol.service, primary: true)
        let list = [
            CBMutableCharacteristic(type: PresenceProtocol.challenge, properties: [.read], value: nil, permissions: read),
            CBMutableCharacteristic(type: PresenceProtocol.response, properties: [.write], value: nil, permissions: write),
            CBMutableCharacteristic(type: PresenceProtocol.enrol, properties: [.write], value: nil, permissions: write),
            CBMutableCharacteristic(type: PresenceProtocol.telemetry, properties: [.write], value: nil, permissions: write),
        ]
        for c in list { chars[c.uuid] = c }
        service.characteristics = list
        peripheral.removeAllServices()
        peripheral.add(service)
    }

    func peripheralManager(_ peripheral: CBPeripheralManager, didAdd service: CBService, error: Error?) {
        if let error { log("add service failed: \(error)"); exit(1) }
        peripheral.startAdvertising([CBAdvertisementDataServiceUUIDsKey: [PresenceProtocol.service],
                                     CBAdvertisementDataLocalNameKey: "tinycrypt"])
    }

    func peripheralManagerDidStartAdvertising(_ peripheral: CBPeripheralManager, error: Error?) {
        if let error { log("advertising failed: \(error)"); exit(1) }
        log("advertising\(opts.encrypt ? " (encryption required)" : ""); \(opts.enrol ? "ENROL mode" : pub == nil ? "no watch enrolled: run with --enrol" : "watch enrolled")")
        arm()
    }

    // MARK: requests

    func peripheralManager(_ peripheral: CBPeripheralManager, didReceiveRead request: CBATTRequest) {
        guard request.characteristic.uuid == PresenceProtocol.challenge else {
            return peripheral.respond(to: request, withResult: .readNotPermitted)
        }
        guard request.offset <= challenge.count else {
            return peripheral.respond(to: request, withResult: .invalidOffset)
        }
        request.value = challenge.subdata(in: request.offset..<challenge.count)
        peripheral.respond(to: request, withResult: .success)
        log("challenge read (+\(ms(since: armedAt))ms since armed)")
    }

    func peripheralManager(_ peripheral: CBPeripheralManager, didReceiveWrite requests: [CBATTRequest]) {
        // A long write arrives as several requests with offsets; reassemble per characteristic.
        var values: [CBUUID: Data] = [:]
        for r in requests.sorted(by: { $0.offset < $1.offset }) {
            values[r.characteristic.uuid, default: Data()].append(r.value ?? Data())
        }
        var result = CBATTError.Code.success
        for (uuid, value) in values {
            switch uuid {
            case PresenceProtocol.enrol: result = enrol(value)
            case PresenceProtocol.response: result = respond(value)
            case PresenceProtocol.telemetry: telemetry(value)
            default: result = .writeNotPermitted
            }
        }
        peripheral.respond(to: requests[0], withResult: result)
        // Telemetry follows the response, so the watch's platform is known once a case is complete.
        if values[PresenceProtocol.telemetry] != nil, let path = opts.vectorPath, let pub, cases.count >= opts.count {
            writeVector(path, pub)
            DispatchQueue.main.asyncAfter(deadline: .now() + 1) { exit(0) }
        }
    }

    func enrol(_ value: Data) -> CBATTError.Code {
        guard opts.enrol else { log("enrol refused: not in --enrol mode"); return .writeNotPermitted }
        guard value.count == 64, (try? P256.Signing.PublicKey(rawRepresentation: value)) != nil else {
            log("enrol refused: not a P-256 X‖Y public key (\(value.count) bytes)")
            return .unlikelyError
        }
        try? FileManager.default.createDirectory(at: stateDir, withIntermediateDirectories: true)
        try? hex(value).write(to: pubFile, atomically: true, encoding: .utf8)
        pub = value
        log("ENROLLED watch key \(hex(value).prefix(16))… -> \(pubFile.path)")
        return .success
    }

    func respond(_ value: Data) -> CBATTError.Code {
        guard let pub, let key = try? P256.Signing.PublicKey(rawRepresentation: pub) else {
            log("response refused: no watch enrolled")
            return .insufficientAuthorization
        }
        let message = challenge
        guard let sig = try? P256.Signing.ECDSASignature(rawRepresentation: value),
              key.isValidSignature(sig, for: message) else {
            log("INVALID response (\(value.count) bytes)")
            return .unlikelyError
        }
        log("VALID signature, +\(ms(since: armedAt))ms since armed\(sigcheck(pub, message, value))")
        cases.append(["name": "watch challenge \(cases.count + 1)",
                      "message": hex(message),
                      "sha256": hex(Data(SHA256.hash(data: message))),
                      "sig": hex(value)])
        arm()
        return .success
    }

    func sigcheck(_ pub: Data, _ message: Data, _ sig: Data) -> String {
        guard let path = opts.sigcheck else { return "" }
        let p = Process()
        p.executableURL = URL(fileURLWithPath: path)
        p.arguments = [hex(pub), hex(message), hex(sig)]
        do { try p.run() } catch { return "; sigcheck failed to run: \(error)" }
        p.waitUntilExit()
        return p.terminationStatus == 0 ? "; sigcheck VALID" : "; sigcheck REJECTED (\(p.terminationStatus))"
    }

    func telemetry(_ value: Data) {
        log("watch telemetry: \(String(decoding: value, as: UTF8.self))")
        if let obj = try? JSONSerialization.jsonObject(with: value) as? [String: Any],
           let context = obj["context"] as? [String: String] {
            watchPlatform = [context["os"], context["model"]].compactMap { $0 }.joined(separator: " ")
        }
    }

    func writeVector(_ path: String, _ pub: Data) {
        let vector: [String: Any] = [
            "description": "P-256 signatures from a real Apple Watch Secure Enclave key via CryptoKit "
                + "signature(for: Data), over BLE presence challenges (CRYPT-11). The signed value is "
                + "SHA-256(message); message = \"tinycrypt-presence-v0\" || nonce(32). "
                + "Formats: pub = X||Y (64 bytes), sig = r||s (64 bytes).",
            "generated": ISO8601DateFormatter().string(from: .now),
            "platform": watchPlatform,
            "pub": hex(pub),
            "source": "secure-enclave",
            "cases": cases,
        ]
        let data = try! JSONSerialization.data(withJSONObject: vector, options: [.prettyPrinted, .sortedKeys])
        try! (data + Data("\n".utf8)).write(to: URL(fileURLWithPath: path))
        log("wrote \(cases.count) cases to \(path)")
    }
}

let standin = Standin(Options(Array(CommandLine.arguments.dropFirst())))
withExtendedLifetime(standin) { RunLoop.main.run() }
