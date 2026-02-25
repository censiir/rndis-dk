#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUNDLE_ID="${BUNDLE_ID:-censiir.rndis-dk}"
TEAM_ID="${TEAM_ID:-T6V8QY9542}"
CONFIGURATION="${CONFIGURATION:-Debug}"
DERIVED_DATA="${DERIVED_DATA:-/tmp/rndis-dk-derived}"
DEXT_PATH="${DEXT_PATH:-${DERIVED_DATA}/Build/Products/${CONFIGURATION}-driverkit/${BUNDLE_ID}.dext}"

if [[ $EUID -ne 0 ]]; then
  echo "Run as root: sudo $0"
  exit 1
fi

if command -v adb >/dev/null 2>&1; then
  echo "Stopping adb server to avoid USB exclusive ownership..."
  adb kill-server || true
fi

if [[ ! -d "${DEXT_PATH}" ]]; then
  echo "Dext not found at ${DEXT_PATH}; building unsigned artifact first..."
  "${ROOT_DIR}/scripts/build_dext.sh" --unsigned
fi

echo "Enabling system extension developer mode (best effort)..."
systemextensionsctl developer on || true

echo "Attempting load via kmutil..."
if kmutil load --bundle-path "${DEXT_PATH}" --load-style start-and-match; then
  echo "kmutil load completed."
else
  echo "kmutil load failed."
  echo "For DriverKit system extension activation, use the signed host app flow:"
  echo "  ./scripts/build_loader_app.sh"
  echo "  ./scripts/open_loader_app.sh"
  echo "Then click Activate in the app for bundle id: ${BUNDLE_ID}."
fi

echo
echo "Current extension state:"
systemextensionsctl list || true

echo
echo "Filtered entry (${BUNDLE_ID}):"
systemextensionsctl list | grep -E "${BUNDLE_ID}|${TEAM_ID}" || true

echo
echo "USB exclusive owner check:"
EXCLUSIVE_OWNER_LINES="$(ioreg -p IOUSB -l -w0 | grep -E 'UsbExclusiveOwner|kUSBProductString' || true)"
if [[ -n "${EXCLUSIVE_OWNER_LINES}" ]]; then
  echo "${EXCLUSIVE_OWNER_LINES}"
else
  echo "No UsbExclusiveOwner entries found."
fi
