// swift-tools-version:5.9
// Generates P-256 test vectors from a real Secure Enclave key (CRYPT-10).
import PackageDescription

let package = Package(
    name: "se-vector",
    platforms: [.macOS(.v13)],
    targets: [
        .executableTarget(name: "se-vector")
    ]
)
