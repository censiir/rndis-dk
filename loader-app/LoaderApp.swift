import SwiftUI

@main
struct RNDISLoaderApp: App {
    @StateObject private var loader = SystemExtensionLoader()

    var body: some Scene {
        WindowGroup {
            ContentView(loader: loader)
        }
        .windowStyle(.automatic)
    }
}
