# Unmerged OPD2413 display experiments

`1410-backlight-ktz8868-retire-firmware-leds-before-bias-setup.patch` is the
grey1 candidate tested on 2026-11-04. It adopts the inherited LED enable state,
fully extinguishes each KTZ8868 string and preserves the live LCD bias.

The actual-function model and kernel/module build passed, and hardware logs
confirmed both strings were switched off before the MDSS scanout retire.
**The visible grey flash remained unchanged.** This patch is therefore outside
the kernel `patches/series`; it is an archived experiment, not a grey-flash fix.
The current test installation still runs this kernel (#32), with normal touch
and Adreno 830 rendering. Images and raw evidence are under ignored
`output/opd2413-stage4/grey1/`.

After applying the candidate explicitly to a matching patched kernel tree:

```sh
python3 porting/oneplus-pad-2-pro/linux/test_ktz8868_handoff.py \
  --kernel-source /path/to/kernel
```

Resume investigation from the remaining physical flash timing across EFI,
early framebuffer and native-panel takeover. Do not infer its cause solely
from successful backlight commands. See dual-boot plan §19.44.
