# OnePlus Pad 2 Pro audio

The source and generated blob describe stereo 48 kHz playback expanded by the
DSP MFC to eight TDM slots, six AW88261 amplifiers, WCD939x microphone capture,
and a separate DisplayPort PCM. DisplayPort is excluded from the default UCM
until its unplugged probing and hotplug behavior are verified.

Sources are derived from `yzddmr6/onepluspad-2pro-mainline` at
`9762c0994e2b5acd672be2ddbda680fd61f1cad6`, using the upstream AudioReach
macros pinned in `source.json`. The sources and blob are BSD-3-Clause.
The kernel dependency is `1408-ASoC-qcom-add-opd2413-tdm-audio.patch`.

With Python 3.12+, m4 and alsatplg (tested: 1.4.21 and 1.2.16):

```sh
curl -fL https://github.com/linux-msm/audioreach-topology/archive/993a17dcb672357998a463a73f120064d6c74f4f.tar.gz \
  -o /tmp/audioreach-topology-993a17dc.tar.gz
python3 audio_topology/oneplus-pad-2-pro/generate.py \
  --archive /tmp/audioreach-topology-993a17dc.tar.gz \
  --output system_files/usr/lib/firmware/qcom/sm8750/OnePlus-Pad-2-Pro-tplg.bin
```

The generator checks the upstream archive and resulting blob hashes before
installing the output. alsatplg reports three undefined backend widgets; they
are supplied by the kernel DAI links, not the topology. The tested blob is
`d2fd46689b0232263380984cf4a23b5261a98ed68e5695b1be96a8c25f47a107`.
Deliberate topology changes require hardware validation and updated hashes in
both `source.json` and the image's `oneplus-pad-2-pro-topology.sha256`.

## Local installation image inputs

Stock ADSP/SoCCP firmware and the AW88261 ACF are private local inputs, not part
of this repository. Extract the firmware from the matching stock images, then:

```sh
python3 porting/oneplus-pad-2-pro/linux/prepare_audio_firmware.py \
  --firmware-dir /path/to/extracted/firmware \
  --aw88261-acf /path/to/stock/odm/firmware/aw882xx_acf.bin
just package kernel
ARMADA_DEVICE=oneplus-pad-2-pro just build localhost/armada-oneplus-pad-2-pro latest
```

The preparation tool validates every input before copying into git-ignored
`system_files/usr/lib/firmware/{qcom/sm8750,awinic}/erhai/` directories. Build
from this local directory so these inputs are in the container build context.
The normal system-files installation copies them into the image; they are
intentionally present in the resulting local image. Their distribution remains
a separate decision from building a local installation image.

`ARMADA_DEVICE=oneplus-pad-2-pro` makes missing or mismatched firmware fatal.
If private OnePlus inputs are staged, a generic build fails until an explicit
OnePlus target is selected. The topology and UCM are checked after installation.
The dracut step uses the image's newly installed kernel/modules and explicitly
includes all verified board firmware and topology, then checks the actual
archive listing. The kernel package hash includes the new patch, so an old
cached kernel cannot satisfy the new source hash.

No `/run` enable flag or saved ALSA state is needed. The UCM driver fallback
matches the DT `CardName`, independent of the bootloader's DMI long name.
It sets six amplifier gains to 270 (-22.5 dB) and DSP volume to Q13 unity.
Vendor speaker protection is unavailable; retain these conservative gains.

Check the build inputs or an unpacked image with:

```sh
ROOT="$PWD/system_files" ARMADA_DEVICE=oneplus-pad-2-pro \
  bash build_files/41-check-oneplus-audio.sh
bash tests/oneplus-audio-image-test.sh
```

These commands validate image contents, not the device's partition layout or
installation method. A new image still needs a temporary boot with a fresh
writable layer, then playback, volume, mute and cold-boot validation before
installation is considered verified.

## Validation status (2026-11-04)

The kernel patch reconstructs all 13 hardware-tested source files exactly
(55 hunks). Regenerating the topology with the pinned archive produces the
shipped blob byte for byte. On the tablet, UCM selects the speakers and
microphones without a DMI-name alias; browser playback, volume and mute were
confirmed. An isolated Armada dracut run with matching kernel/modules included
all 61 required OnePlus firmware/topology files in the resulting archive.

The image test rejects missing/corrupt firmware and incomplete UCM, as well as
private inputs in a generic build. These source and component checks have
passed. A complete rebuilt installation image with a fresh writable layer and
a cold boot has not yet been validated.
