# Galaxy A5 2017 (a5y17lte, Exynos 7880): mainline Linux / postmarketOS

Owner: Jimmy (GitHub android-a5y7lte). Read README.md (status) and
EVIDENCE.md (every hardware finding, in order) before changing anything.

## State (2026-10-19)
- Boot chain: Samsung S-Boot → mainline U-Boot v2026.10 → Linux v7.2 + our
  patches (mainline/patches) and DT (mainline/dts), FIT inside boot.img ramdisk.
- Passed on the phone: M1 console on screen, M2 keys, M3 eMMC read-only,
  M4 USB networking (NCM+ACM; phone 172.16.42.1, PC gets 172.16.42.2 by DHCP;
  `ssh root@172.16.42.1`, blank password; `scp -O` works; telnet on 23).
- Latest image: out/a5y17lte-mainline-m4e-boot.img (built by build-mainline.sh).
- The MUIC (S2MU004, i2c-gpio on gpc1-0/1, addr 0x3d) must be switched to USB
  by `a5-diag muic` or the PC sees nothing (EVIDENCE 18c/18d).

## Next milestone: M5 postmarketOS install
- Jimmy chose target SYSTEM partition. OPEN QUESTION before any write: the
  phone also runs Sailfish OS; a hybris port normally needs Android /system
  (vendor blobs). Confirm Sailfish does not depend on SYSTEM, and make a TWRP
  backup of SYSTEM + BOOT to the PC, before wiping it.
- Start console-only (SSH + apk), UI (Phosh/Plasma Mobile) after touchscreen.

## Rules
- Test images keep the eMMC read-only (BLKROSET). Any eMMC write needs
  Jimmy's explicit OK first.
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
unicorn), then ./build-userspace.sh and ./build-mainline.sh. U-Boot must be
linked with GNU ld (xPack), not ld.lld.
