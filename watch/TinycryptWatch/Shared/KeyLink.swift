// One connection to a key, as async reads and writes (CRYPT-30). Pairing happens on the first access
// to an encrypted characteristic: iOS shows its own "Pair" prompt, and the write waits for it.
import CoreBluetooth
import Foundation

enum KeyLinkError: LocalizedError {
    case bluetooth(String)
    case notFound          // no key in enrol mode nearby
    case unknownKey        // a saved key this \(DeviceName.current) can no longer find (a new phone, or forgotten in Settings)
    case phoneBluetooth    // the phone's own Bluetooth never came up
    case timedOut(String)
    case disconnected(String)
    case pairingLost       // the key or the phone forgot their pairing
    case outdated(CBUUID)
    case refused(String)

    /// For people, not engineers: what to do about it. The detail goes in `debugDescription`.
    var errorDescription: String? {
        switch self {
        case .bluetooth(let why): return "Bluetooth is \(why)."
        case .notFound: return "No key found. Is it plugged in and pulsing magenta?"
        case .unknownKey: return "This \(DeviceName.current) can't find that key any more. Remove it here, then add it again."
        case .phoneBluetooth: return "This \(DeviceName.current)'s Bluetooth isn't ready. Try again in a moment."
        case .timedOut: return "The key didn't answer. Is it plugged in and nearby?"
        case .disconnected: return "The connection to the key dropped. Try again."
        case .pairingLost:
            return "The key and this \(DeviceName.current) no longer recognise each other. In Settings → Bluetooth, forget tinycrypt, then add the key again."
        case .outdated: return "The key needs a firmware update for this."
        case .refused: return "The key said no. Try again, or press the button on the key."
        }
    }

    var debugDescription: String {
        switch self {
        case .timedOut(let step): return "timed out at \(step)"
        case .disconnected(let why), .refused(let why): return why
        case .outdated(let uuid): return "no characteristic \(uuid)"
        default: return errorDescription ?? ""
        }
    }

    /// The errors a stale bond shows up as (the key re-enrolled, or was reset).
    static func from(_ error: Error) -> KeyLinkError {
        if let att = error as? CBATTError,
           att.code == .insufficientAuthentication || att.code == .insufficientEncryption {
            return .pairingLost
        }
        if let cb = error as? CBError, cb.code == .peerRemovedPairingInformation { return .pairingLost }
        return .refused(error.localizedDescription)
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

    /// Connects to a key this \(DeviceName.current) already knows. Never scans: a key it can't find isn't silently
    /// swapped for whatever nearby device advertises the service (it could be told our Wi-Fi password).
    func open(peripheralID: UUID) async throws {
        try await poweredOn()
        guard let p = central.retrievePeripherals(withIdentifiers: [peripheralID]).first else {
            throw KeyLinkError.unknownKey
        }
        peripheral = p
        try await connect(p)
    }

    /// Connects to the closest key heard in `scanFor` seconds: only for adding a key in enrol mode.
    func openForEnrolment(scanFor: TimeInterval = 3) async throws {
        try await poweredOn()
        central.scanForPeripherals(withServices: [PresenceProtocol.service])
        try? await Task.sleep(for: .seconds(scanFor))
        central.stopScan()
        guard let p = peripheral else { throw KeyLinkError.notFound }
        try await connect(p)
    }

    private func poweredOn() async throws {
        do {
            try await wait("Bluetooth", seconds: 10) {
                if self.central.state == .poweredOn { self.resume(nil) }
            }
        } catch KeyLinkError.timedOut {
            throw KeyLinkError.phoneBluetooth
        }
    }

    private func connect(_ p: CBPeripheral) async throws {
        p.delegate = self
        try await wait("connect", seconds: 15) { self.central.connect(p) }
        try await wait("services", seconds: 10) { p.discoverServices([PresenceProtocol.service]) }
    }

    func read(_ uuid: CBUUID, seconds: Double = 15) async throws -> Data {
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
        guard let c = chars[uuid] else { throw KeyLinkError.outdated(uuid) }
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
        case .unauthorized: fail(KeyLinkError.bluetooth("not allowed for tinycrypt: Settings → tinycrypt → Bluetooth"))
        case .poweredOff: fail(KeyLinkError.bluetooth("switched off: turn it on in Control Centre"))
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
        if let cb = error as? CBError, cb.code == .peerRemovedPairingInformation { return fail(KeyLinkError.pairingLost) }
        fail(KeyLinkError.disconnected(error?.localizedDescription ?? "couldn't connect"))
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        if let cb = error as? CBError, cb.code == .peerRemovedPairingInformation { return fail(KeyLinkError.pairingLost) }
        fail(KeyLinkError.disconnected(error?.localizedDescription ?? "by the key"))
    }

    // MARK: - CBPeripheralDelegate

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        if let error { return fail(KeyLinkError.from(error)) }
        guard let service = peripheral.services?.first(where: { $0.uuid == PresenceProtocol.service }) else {
            return fail(KeyLinkError.outdated(PresenceProtocol.service))
        }
        peripheral.discoverCharacteristics(nil, for: service)
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        for c in service.characteristics ?? [] { chars[c.uuid] = c }
        resume(nil)
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        if let error { return fail(KeyLinkError.from(error)) }
        resume(characteristic.value ?? Data())
    }

    func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        if let error { return fail(KeyLinkError.from(error)) }
        resume(nil)
    }
}
