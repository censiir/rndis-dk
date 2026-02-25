import Foundation
import IOKit
import IOKit.usb
import SystemExtensions

@MainActor
final class SystemExtensionLoader: NSObject, ObservableObject {
    let extensionIdentifier = "censiir.rndis-dk"
    private let rndisVendorID = 0x04E8
    private let rndisProductID = 0x6864
    private let shouldAutoActivate: Bool = ProcessInfo.processInfo.arguments.contains("--activate")

    @Published private(set) var logLines: [String] = []
    @Published private(set) var awaitingUserApproval = false
    @Published private(set) var isRequestInFlight = false
    @Published private(set) var embeddedExtensionStatus = ""

    private var rndisMonitor: DispatchSourceTimer?
    private var lastRNDISPresent = false
    private var didRunStartup = false

    var logText: String {
        logLines.joined(separator: "\n")
    }

    func startup() {
        guard !didRunStartup else { return }
        didRunStartup = true

        refreshEmbeddedExtensionStatus()
        startRNDISMonitor()

        if shouldAutoActivate {
            append("Auto-activation requested (--activate)")
            activate()
        }
    }

    func activate() {
        refreshEmbeddedExtensionStatus()
        submit(OSSystemExtensionRequest.activationRequest(forExtensionWithIdentifier: extensionIdentifier,
                                                          queue: .main),
               verb: "activate")
    }

    func deactivate() {
        submit(OSSystemExtensionRequest.deactivationRequest(forExtensionWithIdentifier: extensionIdentifier,
                                                            queue: .main),
               verb: "deactivate")
    }

    func queryProperties() {
        if #available(macOS 12.0, *) {
            submit(OSSystemExtensionRequest.propertiesRequest(forExtensionWithIdentifier: extensionIdentifier,
                                                              queue: .main),
                   verb: "query properties")
        } else {
            append("Properties request requires macOS 12.0+")
        }
    }

    func refreshEmbeddedExtensionStatus() {
        let dextURL = embeddedDextURL()
        if FileManager.default.fileExists(atPath: dextURL.path) {
            embeddedExtensionStatus = "Embedded dext found at: \(dextURL.path)"
        } else {
            embeddedExtensionStatus = "Embedded dext missing: expected \(dextURL.path)"
        }
    }

    func startRNDISMonitor() {
        guard rndisMonitor == nil else { return }

        let timer = DispatchSource.makeTimerSource(queue: .main)
        timer.schedule(deadline: .now(), repeating: .seconds(1))
        timer.setEventHandler { [weak self] in
            guard let self else { return }
            let present = self.isRNDISDevicePresent()
            if present != self.lastRNDISPresent {
                self.lastRNDISPresent = present
                let message = present ? "RNDIS device detected (04e8:6864)" : "RNDIS device disconnected"
                print(message)
                self.append(message)
            }
        }
        rndisMonitor = timer
        timer.resume()
    }

    private func submit(_ request: OSSystemExtensionRequest, verb: String) {
        awaitingUserApproval = false
        isRequestInFlight = true
        request.delegate = self
        append("Submitting request to \(verb) \(extensionIdentifier)")
        OSSystemExtensionManager.shared.submitRequest(request)
    }

    private func append(_ line: String) {
        let formatter = DateFormatter()
        formatter.dateFormat = "HH:mm:ss"
        let stamp = formatter.string(from: Date())
        let entry = "[\(stamp)] \(line)"
        logLines.append(entry)
        print(entry)
    }

    private func isRNDISDevicePresent() -> Bool {
        guard let matching = IOServiceMatching("IOUSBHostDevice") else {
            return false
        }

        var iterator: io_iterator_t = 0
        let result = IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator)
        if result != KERN_SUCCESS {
            return false
        }

        defer { IOObjectRelease(iterator) }
        var service = IOIteratorNext(iterator)
        while service != 0 {
            defer {
                IOObjectRelease(service)
                service = IOIteratorNext(iterator)
            }

            if let vid = ioRegistryNumber(service, key: "idVendor"),
               let pid = ioRegistryNumber(service, key: "idProduct"),
               vid == rndisVendorID,
               pid == rndisProductID {
                return true
            }
        }

        return false
    }

    private func ioRegistryNumber(_ service: io_registry_entry_t, key: String) -> Int? {
        guard let cfValue = IORegistryEntryCreateCFProperty(service, key as CFString,
                                                            kCFAllocatorDefault, 0)?.takeRetainedValue() else {
            return nil
        }

        return (cfValue as? NSNumber)?.intValue
    }

    private func embeddedDextURL() -> URL {
        Bundle.main.bundleURL
            .appendingPathComponent("Contents/Library/SystemExtensions", isDirectory: true)
            .appendingPathComponent("\(extensionIdentifier).dext", isDirectory: true)
    }
}

extension SystemExtensionLoader: OSSystemExtensionRequestDelegate {
    nonisolated func request(_ request: OSSystemExtensionRequest,
                             actionForReplacingExtension existing: OSSystemExtensionProperties,
                             withExtension ext: OSSystemExtensionProperties) -> OSSystemExtensionRequest.ReplacementAction {
        Task { @MainActor in
            append("Replacing existing extension version \(existing.bundleVersion) with \(ext.bundleVersion)")
        }
        return .replace
    }

    nonisolated func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
        Task { @MainActor in
            awaitingUserApproval = true
            append("Request needs user approval in System Settings > Privacy & Security")
        }
    }

    nonisolated func request(_ request: OSSystemExtensionRequest,
                             didFinishWithResult result: OSSystemExtensionRequest.Result) {
        Task { @MainActor in
            isRequestInFlight = false
            switch result {
            case .completed:
                append("Request completed")
            case .willCompleteAfterReboot:
                append("Request will complete after reboot")
            @unknown default:
                append("Request completed with unknown result")
            }
        }
    }

    nonisolated func request(_ request: OSSystemExtensionRequest,
                             didFailWithError error: Error) {
        Task { @MainActor in
            isRequestInFlight = false
            awaitingUserApproval = false
            append("Request failed: \(error.localizedDescription)")
        }
    }

    @available(macOS 12.0, *)
    nonisolated func request(_ request: OSSystemExtensionRequest,
                             foundProperties properties: [OSSystemExtensionProperties]) {
        Task { @MainActor in
            if properties.isEmpty {
                append("Properties query returned no matching extensions")
                return
            }

            for entry in properties {
                append("Found: \(entry.bundleIdentifier) version=\(entry.bundleVersion) enabled=\(entry.isEnabled) awaitingApproval=\(entry.isAwaitingUserApproval) uninstalling=\(entry.isUninstalling)")
            }
        }
    }
}
