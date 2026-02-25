#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

"${ROOT_DIR}/scripts/build_loader_app.sh"
"${ROOT_DIR}/scripts/open_loader_app.sh" --activate

echo "Launched loader app with auto-activation (--activate)."
echo "If prompted, approve in System Settings > Privacy & Security."
