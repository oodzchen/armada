#!/bin/bash
set -euxo pipefail

# Runs after 40-vendor-system-files: the initramfs bundles the armada splash,
# which is not yet installed when the kernel step runs.
KVER="$(ls /usr/lib/modules)"
IMG="/usr/lib/modules/${KVER}/initramfs.img"

# fedora-bootc ships /root -> var/roothome, absent in the build container;
# dracut-install aborts resolving /root without the target.
mkdir -p /var/roothome

# These Duo GPU and ADSP files are not advertised through MODULE_FIRMWARE.
# Include them explicitly for drivers that probe before switching root.
duo_firmware=(
    /usr/lib/firmware/qcom/gen71700_sqe.fw
    /usr/lib/firmware/qcom/gen71700_gmu.bin
    /usr/lib/firmware/qcom/sm7750/retroidpocket/duo/gen70e00_zap.mbn
    /usr/lib/firmware/qcom/sm7750/retroidpocket/duo/adsp.mbn
    /usr/lib/firmware/qcom/sm7750/retroidpocket/duo/adsp_dtb.mbn
)

# These board-specific names come from DT/topology and are not discoverable
# through MODULE_FIRMWARE. Include the verified local inputs before drivers
# can probe; use the same kernel/module tree installed by step 20.
early_firmware=( "${duo_firmware[@]}" )
if [[ "${ARMADA_DEVICE:-}" == oneplus-pad-2-pro ]]; then
    while read -r digest path; do
        [[ -z "${digest}" || "${digest}" == \#* ]] && continue
        early_firmware+=( "/usr/lib/firmware/${path}" )
    done < /usr/share/armada/firmware/oneplus-pad-2-pro.sha256
    early_firmware+=( /usr/lib/firmware/qcom/sm8750/OnePlus-Pad-2-Pro-tplg.bin )
    # The generic A830 microcode is supplied by system_files/linux-firmware;
    # early GPU probing also needs it alongside the board-signed ZAP.
    for name in gen80000_sqe.fw gen80000_gmu.bin gen80000_aqe.fw; do
        path="/usr/lib/firmware/qcom/${name}"
        if [[ -f "${path}" ]]; then
            early_firmware+=( "${path}" )
        elif [[ -f "${path}.xz" ]]; then
            early_firmware+=( "${path}.xz" )
        elif [[ -f "${path}.zst" ]]; then
            early_firmware+=( "${path}.zst" )
        else
            echo "Missing OnePlus GPU firmware: ${name}" >&2
            exit 1
        fi
    done
fi

dracut \
    --force \
    --no-hostonly \
    --reproducible \
    --kver "${KVER}" \
    --add ostree \
    --add armada-splash \
    --add armada-ostree-fallback \
    --install "${early_firmware[*]}" \
    "${IMG}" "${KVER}"

# dracut drops modules silently: fail the build rather than ship without.
# Exact match: a substring test lets armada-splash-launcher pass for the binary.
required=(
    usr/lib/systemd/system/armada-splash-initrd.service \
    usr/lib/systemd/system/dracut-pre-mount.service.d/armada-splash.conf \
    usr/libexec/armada/armada-splash \
    usr/libexec/armada/armada-splash-launcher \
    usr/libexec/armada/device-env \
    usr/share/armada/splash/splash.asp \
    usr/libexec/armada/armada-ostree-fallback \
    usr/lib/systemd/system/ostree-prepare-root.service.d/armada-fallback.conf \
    usr/lib/ostree/ostree-prepare-root
)
required+=( "${early_firmware[@]#/}" )

if ! lsinitrd "${IMG}" | awk '
    BEGIN {
        count = ARGC - 1
        for (i = 1; i <= count; i++) {
            paths[i] = ARGV[i]
            required[ARGV[i]] = 1
        }
        ARGC = 1
    }
    $NF in required { found[$NF] = 1 }
    END {
        for (i = 1; i <= count; i++) {
            if (!(paths[i] in found)) {
                print "ERROR: " paths[i] " missing from initramfs" > "/dev/stderr"
                missing = 1
            }
        }
        exit missing
    }
' "${required[@]}"; then
    dracut --list-modules --kver "${KVER}" | grep -i armada || true
    exit 1
fi

echo "initramfs generated for ${KVER} with armada-splash"
