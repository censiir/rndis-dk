import SwiftUI

struct ContentView: View {
    @ObservedObject var loader: SystemExtensionLoader

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("RNDIS Driver Loader")
                .font(.title2)
                .bold()

            Text("Extension ID: \(loader.extensionIdentifier)")
                .font(.callout)
                .foregroundStyle(.secondary)

            Text(loader.embeddedExtensionStatus)
                .font(.callout)

            HStack(spacing: 10) {
                Button("Activate") {
                    loader.activate()
                }
                .keyboardShortcut(.defaultAction)
                .disabled(loader.isRequestInFlight)

                Button("Deactivate") {
                    loader.deactivate()
                }
                .disabled(loader.isRequestInFlight)

                Button("Query") {
                    loader.queryProperties()
                }
                .disabled(loader.isRequestInFlight)
            }

            if loader.awaitingUserApproval {
                Text("Waiting for user approval in System Settings > Privacy & Security")
                    .font(.footnote)
                    .foregroundStyle(.orange)
            }

            Divider()

            ScrollView {
                Text(loader.logText)
                    .font(.system(.footnote, design: .monospaced))
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .textSelection(.enabled)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .background(Color(nsColor: .textBackgroundColor))
            .overlay(
                RoundedRectangle(cornerRadius: 6)
                    .stroke(Color.secondary.opacity(0.25), lineWidth: 1)
            )
        }
        .padding(16)
        .frame(minWidth: 720, minHeight: 460)
        .onAppear {
            loader.startup()
        }
    }
}
