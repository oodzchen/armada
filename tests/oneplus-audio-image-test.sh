#!/bin/bash
# Test the image gate with staged inputs when available; generic CI always
# exercises target isolation and rejection of missing private firmware.
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
check="${repo}/build_files/41-check-oneplus-audio.sh"
reject() {
    if ROOT="${work}" ARMADA_DEVICE="$1" bash "${check}" >"${work}/check.log" 2>&1; then
        echo "OnePlus image gate accepted $2" >&2
        exit 1
    fi
}
ROOT="${work}" ARMADA_DEVICE='' bash "${check}"
reject oneplus-pad-2-pro 'missing firmware'
mkdir -p "${work}/usr/lib/firmware/awinic/erhai"
reject '' 'private ACF directory in a generic image'
rmdir "${work}/usr/lib/firmware/awinic/erhai"
mkdir -p "${work}/usr/lib/firmware/novatek"
touch "${work}/usr/lib/firmware/novatek/nt36536-xn242.bin"
reject '' 'private touch firmware in a generic image'
rm "${work}/usr/lib/firmware/novatek/nt36536-xn242.bin"
mkdir -p "${work}/usr/lib/firmware/qcom/sm8750/erhai"
reject '' 'private ADSP directory in a generic image'
(
    cd "${repo}/system_files"
    sha256sum --check usr/share/armada/firmware/oneplus-pad-2-pro-topology.sha256
)

if [[ -d "${repo}/system_files/usr/lib/firmware/qcom/sm8750/erhai" ]]; then
    # Independent copies; corrupting fixtures must not modify build inputs.
    mkdir -p "${work}/usr/share"
    cp -a "${repo}/system_files/usr/share/alsa" "${work}/usr/share/"
    cp -a "${repo}/system_files/usr/share/armada" "${work}/usr/share/"
    while read -r digest path; do
        [[ -z "${digest}" || "${digest}" == \#* ]] && continue
        mkdir -p "${work}/usr/lib/firmware/$(dirname "${path}")"
        cp "${repo}/system_files/usr/lib/firmware/${path}" "${work}/usr/lib/firmware/${path}"
    done < "${repo}/system_files/usr/share/armada/firmware/oneplus-pad-2-pro.sha256"
    cp "${repo}/system_files/usr/lib/firmware/qcom/sm8750/OnePlus-Pad-2-Pro-tplg.bin" \
       "${work}/usr/lib/firmware/qcom/sm8750/"
    ROOT="${work}" ARMADA_DEVICE=oneplus-pad-2-pro bash "${check}" >"${work}/pass.log"
    for path in novatek/nt36536-xn242.bin qcom/sm8750/erhai/gen80000_zap.mbn; do
        file="${work}/usr/lib/firmware/${path}"
        mv "${file}" "${file}.saved"
        reject oneplus-pad-2-pro "missing ${path}"
        printf corrupt >"${file}"
        reject oneplus-pad-2-pro "corrupt ${path}"
        mv "${file}.saved" "${file}"
    done
    acf="${work}/usr/lib/firmware/awinic/erhai/aw88261_acf.bin"
    mv "${acf}" "${acf}.saved"
    reject oneplus-pad-2-pro 'missing ACF'
    printf corrupt >"${acf}"
    reject oneplus-pad-2-pro 'corrupt ACF'
    mv "${acf}.saved" "${acf}"
    rm "${work}/usr/share/alsa/ucm2/conf.d/sm8750/sm8750.conf"
    reject oneplus-pad-2-pro 'missing stable UCM matching'
else
    echo 'Private firmware not staged; local positive/corruption checks skipped'
fi
echo 'OnePlus audio image checks passed'
