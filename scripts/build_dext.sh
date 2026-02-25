#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJECT_PATH="${ROOT_DIR}/rndis-dk.xcodeproj"
SCHEME="rndis-dk"
CONFIGURATION="${CONFIGURATION:-Debug}"
DERIVED_DATA="${DERIVED_DATA:-/tmp/rndis-dk-derived}"
UNSIGNED="${UNSIGNED:-0}"

if [[ "${1:-}" == "--unsigned" ]]; then
  UNSIGNED=1
fi

COMMON_ARGS=(
  -project "${PROJECT_PATH}"
  -scheme "${SCHEME}"
  -configuration "${CONFIGURATION}"
  -sdk driverkit
  -destination "generic/platform=DriverKit"
  -derivedDataPath "${DERIVED_DATA}"
  build
)

if [[ "${UNSIGNED}" == "1" ]]; then
  xcodebuild "${COMMON_ARGS[@]}" \
    CODE_SIGNING_ALLOWED=NO \
    CODE_SIGNING_REQUIRED=NO \
    CODE_SIGN_IDENTITY=""
else
  xcodebuild "${COMMON_ARGS[@]}"
fi

DEXT_PATH="${DERIVED_DATA}/Build/Products/${CONFIGURATION}-driverkit/censiir.rndis-dk.dext"
echo "Built dext: ${DEXT_PATH}"
