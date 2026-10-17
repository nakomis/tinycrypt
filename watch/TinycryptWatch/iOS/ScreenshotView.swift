// App Store screenshots (CRYPT-30): the real screens with made-up data, in debug builds only, chosen
// by TC_SCREENSHOT (list | approve | add | wifi). The simulator has no key to talk to.
// watch/ci/screenshots.sh takes them.
#if DEBUG
import SwiftUI

struct ScreenshotView: View {
    @EnvironmentObject private var model: KeysModel
    let screen: String

    var body: some View {
        Group {
            switch screen {
            case "approve":
                if let key = model.keys.first {
                    ApproveView(pending: PendingApproval(key: key, site: "Sign in to github.com?",
                                                         expectedRequest: Data(repeating: 1, count: 32),
                                                         sentAt: nil), onClose: {})
                }
            case "add":
                AddKeyView()
            case "wifi":
                if let key = model.keys.first {
                    NavigationStack {
                        WifiSetupView(key: key, onDone: {}, demoNetworks: [
                            WifiNetwork(ssid: "Home", rssi: -45, secured: true),
                            WifiNetwork(ssid: "Home 5G", rssi: -58, secured: true),
                            WifiNetwork(ssid: "Neighbour", rssi: -78, secured: true),
                            WifiNetwork(ssid: "Café Guest", rssi: -82, secured: false),
                        ])
                        .navigationTitle("Wi-Fi").navigationBarTitleDisplayMode(.inline)
                    }
                }
            default:
                KeysView()
            }
        }
    }
}
#endif
