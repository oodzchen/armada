# OPD2413 kShimC menu memory

This backend remembers the last **confirmed menu entry**, including the
Armada boot menu and USB entry. It reads the journal before displaying the
menu and saves a changed choice before handing off to the payload.

The existing LUN 0 `ARMADA` ESP is FAT16. Its preallocated, contiguous 16 KiB
`KSHIM.CHO` contains two aligned 4 KiB journal pages and an immutable provision
page. Each record has a generation, manifest/partition/provision identity,
entry identity and CRC. Invalid records fall back to the manifest default.
An unchanged choice does not write again.

The reader checks GPT headers and entry CRCs in both copies, the declared
entry count and backup-table location, the unique 512 MiB EFI partition,
complete FAT mirrors, the contiguous file chain, and cross-links from FAT
edges or live directory entries. The provision marker binds the file extent
and partition GUID. A commit repeats these checks and compares the original
pages and provision marker before authorizing exactly two LBAs.

The dedicated writer sends one 4 KiB WRITE(10) with FUA to the older page,
then reads it back. UFS power mode, original queue and Hibern8 are restored
before handoff. A transport fault prevents handoff. The normal U-Boot build
retains its read-only command whitelist; the writer is compiled only for this
explicit kShimC backend. No filesystem metadata is written by kShimC.

## Build and provision

Requires companion kShimC commit `d3d87693172783c8e986aaa7b7fb2d2b290d6217`
in `../kShimC`, an AArch64 GNU toolchain,
CMake, Ninja and Python kconfiglib. Activate the Python build environment:

```sh
python porting/oneplus-pad-2-pro/kshim/build.py \
  --kshim-dir ../kShimC --out output/kshim-choice-dryrun
```

On the running Armada system, create `KSHIM.CHO` exclusively, without replacing
an existing file, and provision it through the mounted ESP:

```sh
python3 -c "f=open('/boot/efi/KSHIM.CHO','xb'); f.write(bytes(16384)); f.close()"
python3 provision_choice.py --record /var/lib/armada/opd2413/choice-provision.json
```

The provisioner accepts an empty file or an already valid provision marker.
It preserves both state pages and verifies raw media with direct I/O, avoiding
the stale buffered block-device cache after a filesystem write. Back up this
file and its receipt before storage maintenance. Moving or recreating the file
requires reprovisioning its extent identity; deleting it disables memory and
returns the menu to the manifest default.

First test the dryrun with `fastboot boot`, changing to a non-default entry so
both UFS sessions run. After verifying the reads and restoration, build with
`--enable-write`. Repack only the shim into the existing boot image, preserving
the Android kernel and all other payloads. Installation must preserve the
existing relocated AVB metadata/footer and verify the complete partition
readback, as described in the dual-boot plan.

The journal is outside `boot_b`, so selecting an entry does not change its
hash. Power interruption can invalidate a state record; the other page or
manifest default provides recovery. The records do not establish hardware
atomic-write guarantees for the UFS device.

Hardware evidence and current installation status are in
`ONEPLUS_PAD2_PRO_DUALBOOT_PLAN.md` §19.44. Raw device logs, GUIDs and images
remain in ignored output directories.

Run the native sanitizer tests with:

```sh
python3 porting/oneplus-pad-2-pro/kshim/test_choice.py
```

The installed version has passed ColorOS and Armada handoff, changed-entry
save/readback, and ordinary reboot with an untouched menu automatically
starting Armada. The common entry-index path also covers the boot-menu and
USB entries; those two remembered choices have not had a separate hardware
launch regression in this session.
