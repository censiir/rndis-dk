#!/usr/bin/env bash
set -euo pipefail

BUNDLE_ID="${BUNDLE_ID:-censiir.rndis-dk}"

log stream --style compact \
  --predicate "subsystem == '${BUNDLE_ID}' OR senderImagePath CONTAINS[c] '${BUNDLE_ID}' OR process CONTAINS[c] 'rndis' OR process CONTAINS[c] 'driverkit' OR eventMessage CONTAINS[c] 'rndis-dk' OR eventMessage CONTAINS[c] 'RNDIS Ethernet'"
