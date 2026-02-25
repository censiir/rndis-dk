#!/usr/bin/env bash
set -euo pipefail

BUNDLE_ID="${BUNDLE_ID:-censiir.rndis-dk}"
TEAM_ID="${TEAM_ID:-T6V8QY9542}"

if [[ $EUID -ne 0 ]]; then
  echo "Run as root: sudo $0"
  exit 1
fi

echo "Attempting unload via kmutil..."
kmutil unload --bundle-identifier "${BUNDLE_ID}" || true

echo "Attempting uninstall via systemextensionsctl..."
systemextensionsctl uninstall "${TEAM_ID}" "${BUNDLE_ID}" || true

echo
echo "Current extension state:"
systemextensionsctl list || true
