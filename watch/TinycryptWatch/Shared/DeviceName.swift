// "iPhone", "iPad" or "watch", for messages that say "this iPhone" rather than "this phone" on an iPad.
#if os(iOS)
import UIKit
#endif

enum DeviceName {
    static let current: String = {
        #if os(iOS)
        UIDevice.current.userInterfaceIdiom == .pad ? "iPad" : "iPhone"
        #else
        "watch"
        #endif
    }()
}
