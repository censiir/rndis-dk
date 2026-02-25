#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJECT_PATH="${ROOT_DIR}/rndis-dk.xcodeproj"
SCHEME="rndis-loader"
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
  -destination "generic/platform=macOS"
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

APP_PATH="${DERIVED_DATA}/Build/Products/${CONFIGURATION}/rndis-loader.app"
DEXT_PATH="${APP_PATH}/Contents/Library/SystemExtensions/censiir.rndis-dk.dext"

echo "Built loader app: ${APP_PATH}"
echo "Embedded dext path: ${DEXT_PATH}"
