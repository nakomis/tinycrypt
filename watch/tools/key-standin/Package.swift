// swift-tools-version:5.9
// Mac stand-in for the key's BLE side (CRYPT-11): a GATT peripheral that issues
// presence challenges and verifies the watch's Secure Enclave signatures.
import PackageDescription

let package = Package(
    name: "key-standin",
    platforms: [.macOS(.v13)],
    targets: [
        .executableTarget(
            name: "key-standin",
            linkerSettings: [
                // Embed an Info.plist: macOS needs a Bluetooth usage description
                // before it will let a command-line tool use CoreBluetooth.
                .unsafeFlags(["-Xlinker", "-sectcreate", "-Xlinker", "__TEXT", "-Xlinker", "__info_plist",
                              "-Xlinker", Context.packageDirectory + "/Info.plist"]),
            ])
    ]
)
