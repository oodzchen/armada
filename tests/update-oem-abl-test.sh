#!/usr/bin/env bash
# armada_repository_allowed: OEM-ABL devices never switch to published images.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=/dev/null
source "$ROOT/system_files/usr/lib/armada/update-lib"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
refused() {
	if armada_repository_allowed "$1"; then echo "FAIL: $1 allowed"; exit 1; fi
}
env_stub() {
	printf '#!/bin/sh\necho ARMADA_DEVICE_ID=%s\necho ARMADA_ABL_MANAGED=%s\n' "$1" "$2" > "$tmp/device-env"
	chmod +x "$tmp/device-env"
}
export ARMADA_DEVICE_ENV="$tmp/device-env"
env_stub ayn-odin-2 1
armada_repository_allowed "$ARMADA_CANONICAL_REGISTRY"
armada_repository_allowed "$ARMADA_LEGACY_REGISTRY"
env_stub oneplus-pad-2-pro 0
refused "$ARMADA_CANONICAL_REGISTRY"
refused "$ARMADA_LEGACY_REGISTRY"
armada_repository_allowed "localhost/armada-opd2413"
# Without a working device-env nothing is allowed (fail closed).
export ARMADA_DEVICE_ENV="$tmp/missing"
refused "$ARMADA_CANONICAL_REGISTRY"
# shellcheck disable=SC2016 # literal shell text
grep -q 'armada_repository_allowed "$repository" || fail' "$ROOT/system_files/usr/libexec/armada/armada-update"
echo 'OEM-ABL devices refuse published Armada images; other devices are unaffected'
