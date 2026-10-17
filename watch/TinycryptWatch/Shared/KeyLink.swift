// One connection to a key, as async reads and writes (CRYPT-30). Pairing happens on the first access
// to an encrypted characteristic: iOS shows its own "Pair" prompt, and the write waits for it.
import CoreBluetooth
import Foundation

enum KeyLinkError: LocalizedError {
    case bluetooth(String)
    case notFound
    case timedOut(String)
    case disconnected(String)
    case missing(CBUUID)
    case refused(String)

    var errorDescription: String? {
        switch self {
        case .bluetooth(let why): return "Bluetooth: \(why)"
        case .notFound: return "No key found. Is it plugged in and pulsing magenta?"
        case .timedOut(let step): return "The key didn't answer (\(step))."
        case .disconnected(let why): return "The key disconnected: \(why)"
        case .missing(let uuid): return "The key's firmware is too old (no \(uuid))."
        case .refused(let why): return "The key refused: \(why)"
        }
    }
}

@MainActor
final class KeyLink: NSObject, @preconcurrency CBCentralManagerDelegate, @preconcurrency CBPeripheralDelegate {
    private var central: CBCentralManager!
    private(set) var peripheral: CBPeripheral?
    private var chars: [CBUUID: CBCharacteristic] = [:]
    private var bestRSSI = Int.min

    // At most one operation is outstanding.
    private var waiting: CheckedContinuation<Any?, Error>?
    private var step = ""
    private var timeout: Task<Void, Never>?

    override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: nil)
    }

    /// Connects to the key with this identifier (saved when it was added), or else to the strongest key
    /// heard in `scanFor` seconds (a key in enrol mode, being added).
    func open(peripheralID: UUID?, scanFor: TimeInterval = 3) async throws {
        try await wait("Bluetooth", seconds: 10) {
            if self.central.state == .poweredOn { self.resume(nil) }
        }
        if let id = peripheralID, let p = central.retrievePeripherals(withIdentifiers: [id]).first {
            peripheral = p
        } else {
            central.scanForPeripherals(withServices: [PresenceProtocol.service])
            try? await Task.sleep(for: .seconds(scanFor))
            central.stopScan()
            guard peripheral != nil else { throw KeyLinkError.notFound }
        }
        let p = peripheral!
        p.delegate = self
        try await wait("connect", seconds: 15) { self.central.connect(p) }
        try await wait("services", seconds: 10) { p.discoverServices([PresenceProtocol.service]) }
    }

    func read(_ uuid: CBUUID, seconds: Double = 30) async throws -> Data {
        let c = try characteristic(uuid)
        return try await wait("read", seconds: seconds) { self.peripheral?.readValue(for: c) } as? Data ?? Data()
    }

    /// With a response, so it returns once the key has accepted it (or throws with the key's refusal).
    /// The first encrypted write may wait on iOS's pairing prompt, hence the long default.
    func write(_ uuid: CBUUID, _ value: Data, seconds: Double = 60) async throws {
        let c = try characteristic(uuid)
        _ = try await wait("write", seconds: seconds) { self.peripheral?.writeValue(value, for: c, type: .withResponse) }
    }

    func readJSON<T: Decodable>(_ uuid: CBUUID, as: T.Type) async throws -> T {
        try JSONDecoder().decode(T.self, from: try await read(uuid))
    }

    func writeJSON(_ uuid: CBUUID, _ object: [String: Any]) async throws {
        try await write(uuid, try JSONSerialization.data(withJSONObject: object))
    }

    func close() {
        timeout?.cancel()
        if let peripheral { central.cancelPeripheralConnection(peripheral) }
        fail(KeyLinkError.disconnected("closed"))
    }

    // MARK: - plumbing

    private func characteristic(_ uuid: CBUUID) throws -> CBCharacteristic {
        guard let c = chars[uuid] else { throw KeyLinkError.missing(uuid) }
        return c
    }

    @discardableResult
    private func wait(_ step: String, seconds: Double, start: @escaping () -> Void) async throws -> Any? {
        precondition(waiting == nil, "one KeyLink operation at a time")
        self.step = step
        return try await withCheckedThrowingContinuation { cont in
            waiting = cont
            timeout = Task { @MainActor in
                try? await Task.sleep(for: .seconds(seconds))
                if !Task.isCancelled { self.fail(KeyLinkError.timedOut(step)) }
            }
            start()
        }
    }

    private func resume(_ value: Any?) {
        timeout?.cancel()
        let cont = waiting
        waiting = nil
        cont?.resume(returning: value)
    }

    private func fail(_ error: Error) {
        timeout?.cancel()
        let cont = waiting
        waiting = nil
        cont?.resume(throwing: error)
    }

    // MARK: - CBCentralManagerDelegate

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn: if step == "Bluetooth" { resume(nil) }
        case .unauthorized: fail(KeyLinkError.bluetooth("not allowed: Settings → tinycrypt → Bluetooth"))
        case .poweredOff: fail(KeyLinkError.bluetooth("switched off"))
        case .unsupported: fail(KeyLinkError.bluetooth("not supported on this device"))
        default: break
        }
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        // Several keys nearby: the closest is the one being held.
        if RSSI.intValue > bestRSSI {
            bestRSSI = RSSI.intValue
            self.peripheral = peripheral
        }
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        resume(nil)
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        fail(KeyLinkError.disconnected(error?.localizedDescription ?? "couldn't connect"))
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        fail(KeyLinkError.disconnected(error?.localizedDescription ?? "by the key"))
    }

    // MARK: - CBPeripheralDelegate

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let service = peripheral.services?.first(where: { $0.uuid == PresenceProtocol.service }) else {
            return fail(KeyLinkError.missing(PresenceProtocol.service))
        }
        peripheral.discoverCharacteristics(nil, for: service)
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        for c in service.characteristics ?? [] { chars[c.uuid] = c }
        resume(nil)
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        if let error { return fail(KeyLinkError.refused(error.localizedDescription)) }
        resume(characteristic.value ?? Data())
    }

    func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        if let error { return fail(KeyLinkError.refused(error.localizedDescription)) }
        resume(nil)
    }
}
