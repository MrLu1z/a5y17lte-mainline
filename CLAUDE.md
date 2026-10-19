# Galaxy A5 2017 (a5y17lte, Exynos 7880): mainline Linux / postmarketOS

Owner: Jimmy (GitHub android-a5y7lte). Read README.md (status) and
EVIDENCE.md (every hardware finding, in order) before changing anything.

## State (2026-10-19)
- Boot chain: Samsung S-Boot → mainline U-Boot v2026.10 → Linux v7.2 + our
  patches (mainline/patches) and DT (mainline/dts), FIT inside boot.img ramdisk.
- Passed on the phone: M1 console on screen, M2 keys, M3 eMMC read-only,
  M4 USB networking (NCM+ACM; phone 172.16.42.1, PC gets 172.16.42.2 by DHCP;
  `ssh root@172.16.42.1`, blank password; `scp -O` works; telnet on 23).
- M5a passed: distro kernel config, all partitions read-only (force_ro), SYSTEM = mmcblk0p19 (4848 MiB).
- Latest image: out/a5y17lte-mainline-m5a-boot.img (built by build-mainline.sh,
  reproducible: same commit -> same bytes).
- The MUIC (S2MU004, i2c-gpio on gpc1-0/1, addr 0x3d) must be switched to USB
  by `a5-diag muic` or the PC sees nothing (EVIDENCE 18c/18d).

## Next milestone: M5 postmarketOS install
- Target: SYSTEM partition (Jimmy's choice, 2026-10-19). Sailfish OS is
  installed (rootfs in USERDATA); Jimmy has the Sailfish boot.img and the
  original install files, and said it is fine if Sailfish breaks: he can
  reinstall. Still: don't touch USERDATA, and ask before each eMMC write.
- Start console-only (SSH + apk), UI (Phosh/Plasma Mobile) after touchscreen.

### Where we are (end of 2026-10-19)
- M5a PASSED on the phone (EVIDENCE 19a): SYSTEM = mmcblk0p19, 5083496448 B;
  EFS = mmcblk0p3, 20971520 B; USERDATA = mmcblk0p25.
- M5b BUILT, not on the phone yet: ./build-pmos.sh ->
  build/pmos/samsung-a5y17lte-root.img, 686817280 B, sha256 06e88091addeafbb…
  (see the .sha256 file), ext4 "pmOS_root", e2fsck clean, OpenRC (pinned:
  pmOS console UI now defaults to systemd), sshd + unudhcpd.usb0 + nftables,
  getty on ttyGS0, user "user" with ~/.ssh/id_ed25519.pub, password 147147
  (placeholder; change with passwd). os-release says "Nura edge".
- M5c steps 1-2 DONE (TWRP, read-only): by-name
  /dev/block/platform/13540000.dwmmc0/by-name/{EFS,SYSTEM} -> p3/p19, sizes
  match. Backups in ~/a5-backup (NOT in git; EFS has the IMEI):
  EFS-p3.img sha256 5efb6122…, SYSTEM-p19.img sha256 17530cf1… (full list in
  ~/a5-backup/SHA256SUMS; phone and PC hashes match; e2fsck -fn clean).
- M5c step 3 DONE (EVIDENCE 21): root.img written to by-name/SYSTEM; read-back
  sha256 = 06e88091…, e2fsck 1.47 clean on the PC (TWRP's e2fsck 1.43 can't:
  orphan_file). EFS unchanged at 5f6cfe57… (it had changed before the write:
  modem nv_data, presumably a Sailfish boot; second backup
  EFS-p3-20261019-1440.img). TWRP mounts SYSTEM (/system_root) and EFS at
  startup, so later SYSTEM hashes change; check with e2fsck instead.
- Rollback: `adb exec-in "dd of=<by-name>/SYSTEM bs=4M" < ~/a5-backup/SYSTEM-p19.img`
  (5 GB, won't fit in TWRP /tmp), then check for sha256 17530cf1….
- android-w0 merged into main (d3b6283).
- M5d BUILT (EVIDENCE 22), phone test next: out/a5y17lte-mainline-m5d-boot.img,
  sha256 7ab5ddd7…. Boots Nura from SYSTEM ro (force_ro=1) under a tmpfs
  overlay, so no eMMC writes; hold Volume Down at the 3 s prompt -> rescue image.
- Then M5e: read-write root, resize2fs, apk via PC NAT (needs Jimmy's OK).

## Roadmap after M5 (Jimmy, 2026-10-19)
Two OSes on ONE mainline kernel + DT: Nura/postmarketOS (primary, on SYSTEM)
and, later, open-source Android (AOSP) — no Samsung vendor blobs, which only
work with Samsung's 3.18 kernel.
- Shared kernel milestones, in order: touchscreen → Phosh on Nura; real 7880
  CMU clock driver; display (DECON/DSI, mandatory for Android's
  drm_hwcomposer); GPU (Mali-T830 via Panfrost, after G3D power domain, clock
  and vdd_g3d — MrLu1z's gpu-test branch has the node values but does not
  build and lacks power-domain/clock/regulator); battery, audio, Wi-Fi.
- Then binder/Android kernel options → Waydroid on Nura (first Android apps).
- Then a GloDroid-style AOSP device for a5y17lte (Mesa/Panfrost,
  drm_hwcomposer, minigbm) on the same kernel, on a partition/SD card that
  does not replace Nura on SYSTEM.
- Known limits for both OSes: no modem (Shannon, no mainline driver → no
  calls/SMS/data) and no camera, for now.

## Rules
- Test images keep the eMMC read-only (/sys/block/mmcblk0/force_ro = 1;
  BLKROSET alone never covered the partitions, EVIDENCE 19). Any eMMC write
  needs Jimmy's explicit OK first.
- Never touch ~/hadk or ~/sfos (Sailfish build trees).
- Work in small milestones; each one is flashed and tested on the phone
  (TWRP → Install Image → Boot), with a photo/log recorded in EVIDENCE.md.
- Kernel patches: `Assisted-by: Claude:<model>` trailer, no AI Signed-off-by
  (Documentation/process/coding-assistants.rst); run checkpatch.
- Commits: author Jimmy <fakeandroit@gmail.com>.
- iOS/XNU direction is closed; don't resume it. uniLoader forbids AI agents
  (its AGENTS.md); don't modify it.

## Build
./setup.sh (clones upstream/: linux, u-boot, k7880 reference, xPack GCC,
unicorn, pmaports, pmbootstrap), then ./build-userspace.sh and
./build-mainline.sh (boot image, reproducible), then ./build-pmos.sh (pmOS
rootfs; needs sudo). U-Boot must be linked with GNU ld (xPack), not ld.lld.
