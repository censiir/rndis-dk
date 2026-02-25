#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIGURATION="${CONFIGURATION:-Debug}"
DERIVED_DATA="${DERIVED_DATA:-/tmp/rndis-dk-derived}"
APP_PATH="${DERIVED_DATA}/Build/Products/${CONFIGURATION}/rndis-loader.app"
AUTO_ACTIVATE=0

if [[ "${1:-}" == "--activate" ]]; then
  AUTO_ACTIVATE=1
fi

if [[ ! -d "${APP_PATH}" ]]; then
  "${ROOT_DIR}/scripts/build_loader_app.sh"
fi

if [[ "${AUTO_ACTIVATE}" -eq 1 ]]; then
  open "${APP_PATH}" --args --activate
else
  open "${APP_PATH}"
fi
