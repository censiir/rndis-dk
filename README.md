# rndis-dk (HoRNDIS-style DriverKit USB RNDIS)

## ATTENTION! This project is for personal use and will only be fixed as needed by the maintainer. There is no guarantee of bug fixes or updates. You are on your own.

This project implements a DriverKit USB RNDIS driver extension (`censiir.rndis-dk.dext`) with a HoRNDIS-compatible control-plane initialization flow:

- CDC encapsulated control transport
- RNDIS initialize/query/set/keepalive transactions
- USB control/data interface and endpoint discovery
- RNDIS halt during teardown

## Project Layout

- Driver source: `rndis-dk/rndis_dk.cpp`
- Driver interface: `rndis-dk/rndis_dk.iig`
- Driver personality: `rndis-dk/Info.plist`
- Build/load helpers: `scripts/*.sh`

## Build

## Build in Xcode

1. Open `/Users/ubayd/rndis-dk/rndis-dk.xcodeproj` in Xcode.
2. Select scheme `rndis-loader` to build/run the host app that embeds and activates the dext.
3. Use destination `My Mac` and press Run.
4. In the app, click `Activate` (or launch with `--activate`).

If you only want to build the DriverKit extension binary, select scheme `rndis-dk` and use destination `Any DriverKit Device (arm64)` (or generic DriverKit destination).

Signed build:

```bash
./scripts/build_dext.sh
```

Unsigned build (useful for compile verification only):

```bash
./scripts/build_dext.sh --unsigned
```

Default artifact path:

```text
/tmp/rndis-dk-derived/Build/Products/Debug-driverkit/censiir.rndis-dk.dext
```

Build the SwiftUI loader app (this embeds the dext into the app bundle):

```bash
./scripts/build_loader_app.sh
```

Unsigned loader app build (compile verification):

```bash
./scripts/build_loader_app.sh --unsigned
```

Default app artifact path:

```text
/tmp/rndis-dk-derived/Build/Products/Debug/rndis-loader.app
```

## User-Space RNDIS Probe (Before DriverKit)

Use the dependency-free probe to validate raw RNDIS control traffic first:

```bash
python3 tools/rndis_probe.py --self-test
python3 tools/rndis_probe.py --list
```

If your RNDIS device appears in `--list`, run the full control sequence:

```bash
python3 tools/rndis_probe.py --device-index 0
```

This sends:

1. `REMOTE_NDIS_INITIALIZE_MSG`
2. `REMOTE_NDIS_QUERY_MSG` for MAC address OIDs
3. `REMOTE_NDIS_SET_MSG` for packet filter
4. `REMOTE_NDIS_KEEPALIVE_MSG`

Optional cleanup:

```bash
python3 tools/rndis_probe.py --device-index 0 --halt-on-exit
```

Note: the probe uses `libusb` via `ctypes` (`/opt/homebrew/lib/libusb-1.0.dylib`).

## Load / Unload

Preferred load path (recommended):

1. Build signed loader app: `./scripts/build_loader_app.sh`
2. Launch it: `./scripts/open_loader_app.sh`
3. Click `Activate` in the app.
4. If prompted, approve in `System Settings > Privacy & Security`.

Low-level development load path (root required, may be rejected by policy):

```bash
sudo ./scripts/load_dext.sh
```

Unload (root required):

```bash
sudo ./scripts/unload_dext.sh
```

## Important Activation Notes

DriverKit `.dext` deployment on macOS requires valid signing and entitlements.

At minimum, the dext signing profile must include USB transport entitlement:

- `com.apple.developer.driverkit.transport.usb`

For production-like activation, use a signed host app that embeds this dext and submits `OSSystemExtensionRequest` activation for:

- Bundle ID: `censiir.rndis-dk`
- Team ID: `T6V8QY9542`
- Host app entitlement: `com.apple.developer.system-extension.install`

The included `rndis-loader` SwiftUI app is the host app for this flow. It issues activate/deactivate/property requests through `OSSystemExtensionManager`.

The helper `load_dext.sh` script tries a low-level `kmutil` path for development workflows; if macOS policy rejects it, activate through the host app + System Extensions flow.

## Verify State

List installed/active system extensions:

```bash
systemextensionsctl list
```

Follow driver logs:

```bash
log stream --style compact --predicate 'senderImagePath CONTAINS "censiir.rndis-dk" OR process CONTAINS "rndis"'
```
