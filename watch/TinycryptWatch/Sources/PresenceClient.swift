// One BLE exchange with the key, as central: enrol (write our public key) or
// approve (read the challenge, sign it, write the signature back). Records how
// long each step takes, measured from `start` (the tap on Approve).
import CoreBluetooth
import CryptoKit
import Foundation

struct PresenceReport: Codable {
    var mode: String
    var route: String = "?"
    var context: [String: String] = [:]
    var ms: [String: Int] = [:]
    var steps: [String] = []
    var error: String?
}

final class PresenceClient: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    enum Mode: String { case enrol, approve }
    enum Route: String { case retrieve, scan }

    static let peripheralIDKey = "enrolledPeripheralID"
    static let timeout: Duration = .seconds(20)

    private let mode: Mode
    private let preferredRoute: Route
    private let start: ContinuousClock.Instant
    private var central: CBCentralManager?
    private var peripheral: CBPeripheral?
    private var chars: [CBUUID: CBCharacteristic] = [:]
    private var report: PresenceReport
    private var continuation: CheckedContinuation<PresenceReport, Never>?
    private var finished = false

    init(mode: Mode, route: Route, start: ContinuousClock.Instant, context: [String: String]) {
        self.mode = mode
        self.preferredRoute = route
        self.start = start
        self.report = PresenceReport(mode: mode.rawValue, context: context)
    }

    /// Always returns; failures are recorded in the report's `error`.
    @MainActor
    func run() async -> PresenceReport {
        await withCheckedContinuation { cont in
            continuation = cont
            mark("start")
            central = CBCentralManager(delegate: self, queue: nil)
            Task { @MainActor in
                try? await Task.sleep(for: Self.timeout)
                self.finish(error: "timed out after \(Self.timeout)")
            }
        }
    }

    // MARK: - steps

    private func mark(_ step: String) {
        let elapsed = ContinuousClock.now - start
        let ms = Int(elapsed.components.seconds * 1000 + elapsed.components.attoseconds / 1_000_000_000_000_000)
        report.ms[step] = ms
        report.steps.append("\(step) +\(ms)ms")
    }

    private func finish(error: String? = nil) {
        guard !finished else { return }
        finished = true
        report.error = error
        if let error { report.steps.append("ERROR \(error)") }
        if let central {
            central.stopScan()
            if let peripheral { central.cancelPeripheralConnection(peripheral) }
        }
        continuation?.resume(returning: report)
        continuation = nil
    }

    private func connect(_ p: CBPeripheral) {
        peripheral = p
        p.delegate = self
        central?.connect(p)
    }

    // MARK: - CBCentralManagerDelegate

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn:
            mark("poweredOn")
            if preferredRoute == .retrieve,
               let id = UserDefaults.standard.string(forKey: Self.peripheralIDKey).flatMap(UUID.init),
               let p = central.retrievePeripherals(withIdentifiers: [id]).first {
                report.route = Route.retrieve.rawValue
                connect(p)
            } else {
                report.route = Route.scan.rawValue
                central.scanForPeripherals(withServices: [PresenceProtocol.service])
            }
        case .unauthorized:
            finish(error: "Bluetooth not authorised")
        case .poweredOff:
            finish(error: "Bluetooth off")
        case .unsupported:
            finish(error: "Bluetooth LE unsupported")
        default:
            break
        }
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        guard self.peripheral == nil else { return }
        mark("discovered(rssi \(RSSI))")
        central.stopScan()
        connect(peripheral)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        mark("connected")
        peripheral.discoverServices([PresenceProtocol.service])
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        finish(error: "connect failed: \(error?.localizedDescription ?? "unknown")")
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        finish(error: "disconnected: \(error?.localizedDescription ?? "by peer")")
    }

    // MARK: - CBPeripheralDelegate

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let service = peripheral.services?.first(where: { $0.uuid == PresenceProtocol.service }) else {
            return finish(error: "service missing: \(error?.localizedDescription ?? "not advertised")")
        }
        peripheral.discoverCharacteristics(nil, for: service)
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        for c in service.characteristics ?? [] { chars[c.uuid] = c }
        mark("characteristics")
        switch mode {
        case .enrol:
            guard let c = chars[PresenceProtocol.enrol] else { return finish(error: "no enrol characteristic") }
            do {
                peripheral.writeValue(KeyStore.publicKeyXY(try KeyStore.key()), for: c, type: .withResponse)
            } catch {
                finish(error: "key: \(error)")
            }
        case .approve:
            guard let c = chars[PresenceProtocol.challenge] else { return finish(error: "no challenge characteristic") }
            peripheral.readValue(for: c)
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        guard characteristic.uuid == PresenceProtocol.challenge else { return }
        if let error { return finish(error: "challenge read: \(error.localizedDescription)") }
        mark("challengeRead")
        let challenge = characteristic.value ?? Data()
        // Sign only a well-formed presence challenge, never arbitrary bytes.
        guard challenge.count == PresenceProtocol.domainTag.count + PresenceProtocol.nonceLength,
              challenge.starts(with: PresenceProtocol.domainTag) else {
            return finish(error: "malformed challenge (\(challenge.count) bytes)")
        }
        guard let response = chars[PresenceProtocol.response] else { return finish(error: "no response characteristic") }
        do {
            let sig = try KeyStore.key().signature(for: challenge).rawRepresentation
            mark("signed")
            peripheral.writeValue(sig, for: response, type: .withResponse)
        } catch {
            finish(error: "sign: \(error)")
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        if let error { return finish(error: "write \(characteristic.uuid): \(error.localizedDescription)") }
        switch characteristic.uuid {
        case PresenceProtocol.enrol:
            mark("enrolled")
            UserDefaults.standard.set(peripheral.identifier.uuidString, forKey: Self.peripheralIDKey)
            finish()
        case PresenceProtocol.response:
            mark("responseWritten")
            // Spike only: send our timings so they land in the key's log too.
            // Steps are dropped to stay inside one 512-byte ATT value.
            var compact = report
            compact.steps = []
            if let t = chars[PresenceProtocol.telemetry], let json = try? JSONEncoder().encode(compact),
               json.count <= 512 {
                peripheral.writeValue(json, for: t, type: .withResponse)
            } else {
                finish()
            }
        case PresenceProtocol.telemetry:
            finish()
        default:
            break
        }
    }
}
