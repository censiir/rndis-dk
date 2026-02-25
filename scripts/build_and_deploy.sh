#!/bin/bash
# Build, re-sign (fix USB entitlements), and deploy the RNDIS driver extension
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
DERIVED_DATA="$HOME/Library/Developer/Xcode/DerivedData"

# Find the DerivedData directory for our project
DD_DIR=$(find "$DERIVED_DATA" -maxdepth 1 -name "rndis-dk-*" -type d | head -1)
if [[ -z "$DD_DIR" ]]; then
    echo "ERROR: DerivedData not found. Build the project in Xcode first."
    exit 1
fi

DEXT_PATH="$DD_DIR/Build/Products/Debug-driverkit/censiir.rndis-dk.dext"
APP_PATH="$DD_DIR/Build/Products/Debug/rndis-loader.app"
DEXT_IN_APP="$APP_PATH/Contents/Library/SystemExtensions/censiir.rndis-dk.dext"
ENTITLEMENTS="$SCRIPT_DIR/rndis-dk-signing.entitlements"
SIGNING_ID="F5138C089D74323A51B6194FF91B73DE455CBD1E"

echo "=== Step 1: Building rndis-dk dext ==="
xcodebuild -project "$PROJECT_DIR/rndis-dk.xcodeproj" \
    -scheme "rndis-dk" \
    -configuration Debug \
    -derivedDataPath "$DD_DIR" \
    build 2>&1 | tail -5

echo ""
echo "=== Step 2: Building rndis-loader app ==="
xcodebuild -project "$PROJECT_DIR/rndis-dk.xcodeproj" \
    -scheme "rndis-loader" \
    -configuration Debug \
    -derivedDataPath "$DD_DIR" \
    build 2>&1 | tail -5

echo ""
echo "=== Step 3: Re-signing dext with correct USB entitlements ==="
codesign --sign "$SIGNING_ID" \
    --entitlements "$ENTITLEMENTS" \
    --force --timestamp=none \
    --generate-entitlement-der \
    "$DEXT_PATH"
echo "Re-signed standalone dext"

echo ""
echo "=== Step 4: Copying re-signed dext into app bundle ==="
rm -rf "$DEXT_IN_APP"
cp -R "$DEXT_PATH" "$DEXT_IN_APP"
echo "Copied dext into app"

echo ""
echo "=== Step 5: Re-signing app bundle ==="
# Re-sign just the app (not --deep, to preserve dext entitlements)
codesign --sign "$SIGNING_ID" \
    --force --timestamp=none \
    "$APP_PATH"
echo "Re-signed app"

echo ""
echo "=== Step 6: Verifying entitlements ==="
USB_ENT=$(codesign -d --entitlements :- "$DEXT_IN_APP" 2>&1 | grep -o 'idVendor.*idProduct' | head -1)
if [[ -n "$USB_ENT" ]]; then
    echo "✓ USB entitlements verified (VID/PID present)"
else
    echo "✗ ERROR: USB entitlements are still empty!"
    exit 1
fi

echo ""
echo "=== Done! ==="
echo "App path: $APP_PATH"
echo ""
echo "Next steps:"
echo "  1. Open the loader app:  open \"$APP_PATH\""
echo "  2. Click Deactivate (if previously activated)"
echo "  3. Click Activate"
echo "  4. Monitor logs:  log stream --predicate 'sender contains \"rndis\" OR message contains \"RNDIS\"' --level debug"
