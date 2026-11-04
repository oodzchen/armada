#!/bin/bash
set -euo pipefail

# Also usable against an unpacked image: ROOT=/path ARMADA_DEVICE=... bash ...
root="$(cd "${ROOT:-/}" && pwd)"
manifest="${root}/usr/share/armada/firmware/oneplus-pad-2-pro.sha256"
firmware="${root}/usr/lib/firmware"
if [[ "${ARMADA_DEVICE:-}" != oneplus-pad-2-pro ]]; then
    if [[ -d "${firmware}/qcom/sm8750/erhai" ||
          -d "${firmware}/awinic/erhai" ]]; then
        echo 'Private OnePlus firmware requires ARMADA_DEVICE=oneplus-pad-2-pro' >&2
        exit 1
    fi
    exit 0
fi

(cd "${firmware}"; sha256sum --check "${manifest}")
(cd "${root:-/}"; sha256sum --check usr/share/armada/firmware/oneplus-pad-2-pro-topology.sha256)
ucm="${root}/usr/share/alsa/ucm2"
for path in conf.d/sm8750/sm8750.conf conf.d/sm8750/OnePlus-Pad-2-Pro.conf \
    Qualcomm/sm8750/OnePlus-Pad-2-Pro/HiFi.conf \
    Qualcomm/sm8750/OnePlus-Pad-2-Pro/Speaker.conf; do
    test -s "${ucm}/${path}" || { echo "Missing OnePlus UCM: ${path}" >&2; exit 1; }
done
if grep -q 'speaker-enabled' "${ucm}/Qualcomm/sm8750/OnePlus-Pad-2-Pro/HiFi.conf"; then
    echo 'OnePlus speaker UCM still depends on a runtime enable flag' >&2
    exit 1
fi
echo 'OnePlus audio firmware, topology and UCM verified'
