#!/usr/bin/env bash
# ufs-guard against a fake sysfs: which UFS block devices it makes read-only.
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
guard="$repo/system_files/usr/libexec/armada/ufs-guard"
rules="$repo/system_files/usr/lib/udev/rules.d/90-armada-ufs-guard.rules"
t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT
mkdir -p "$t/bin" "$t/sys/class/block" "$t/sys/firmware/devicetree/base"
printf '#!/bin/sh\necho "$*" >> "%s/locked"\n' "$t" > "$t/bin/blockdev"
printf '#!/bin/sh\n:\n' > "$t/bin/logger"
chmod +x "$t/bin/blockdev" "$t/bin/logger"
disk() {  # NAME LUN PARTITION...
	local name=$1 lun=$2 dev; shift 2
	dev="$t/sys/devices/platform/soc@0/1d84000.ufs/host0/target0:0:0/0:0:0:$lun"
	mkdir -p "$dev/block/$name"
	ln -s "$dev" "$dev/block/$name/device"
	ln -s "$dev/block/$name" "$t/sys/class/block/$name"
	for p in "$@"; do
		mkdir -p "$dev/block/$name/$p"
		ln -s "$dev/block/$name/$p" "$t/sys/class/block/$p"
	done
}
disk sda 0 sda15 sda16 sda17 sda18 sda19
disk sde 4 sde3
run() {  # MODEL DEVTYPE NAME [PARTLABEL]
	printf '%s\0' "$1" > "$t/sys/firmware/devicetree/base/model"
	rm -f "$t/locked"
	env -i PATH="$t/bin:/usr/bin:/bin" UFS_GUARD_SYSFS="$t/sys" DEVTYPE="$2" \
		${4:+ID_PART_ENTRY_NAME="$4"} sh "$guard" "/dev/$3"
	cat "$t/locked" 2>/dev/null || true
}
check() {  # EXPECTED MODEL DEVTYPE NAME [PARTLABEL]
	local expected=$1 got; shift
	got=$(run "$@")
	[ "$got" = "$expected" ] || { echo "FAIL: $* -> '$got', expected '$expected'"; exit 1; }
}
pad='OnePlus Pad 2 Pro'
check '' "$pad" disk sda
check '--setro /dev/sda15' "$pad" partition sda15 userdata
check '' "$pad" partition sda16 ARMADA
check '' "$pad" partition sda17 ARMADA_BOOT
check '' "$pad" partition sda18 ARMADA_ROOT
check '--setro /dev/sda19' "$pad" partition sda19
check '--setro /dev/sda19' "$pad" partition sda19 ARMADA_ROOTX
check '--setro /dev/sde' "$pad" disk sde
check '--setro /dev/sde3' "$pad" partition sde3 ARMADA
check '' 'KONKR Pocket FIT Elite' partition sda15 userdata
check '' 'KONKR Pocket FIT Elite' disk sde
check '' "$pad" '' sda
# The rule hands only UFS sd* block devices to the helper, after
# 60-persistent-storage (which provides ID_PART_ENTRY_NAME).
grep -q 'DEVPATH!="\*/1d84000.ufs/\*"' "$rules"
# shellcheck disable=SC2016 # literal udev substitution
grep -q 'RUN+="/usr/libexec/armada/ufs-guard $devnode"' "$rules"
[ "$(basename "$rules" | cut -c1-2)" -gt 60 ]
[ -x "$guard" ]
echo 'ufs-guard: Armada partitions on LUN0 writable, everything else on UFS read-only, other devices untouched'
