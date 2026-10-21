# a5y17lte-mainline-exynos7880-gpu

> **Archived (EOL).** Development paused: no hardware to test the remaining GPU work. Code is complete and builds cleanly on v7.2.0; M1–M5a verified on device (`EVIDENCE.md`). Forks welcome.

> **Note:** this is a fork of [`android-a5y7lte/xnu-a5y17lte`](https://github.com/android-a5y7lte/xnu-a5y17lte) that adds the Mali-T830 GPU node, the `CMU_G3D` clock driver, and the `pd_g3d` power domain for the Exynos 7880. **Not tested on hardware.** The mainline kernel boots (M1–M5a passed on the real phone, see [`EVIDENCE.md`](EVIDENCE.md)), but the GPU has not been probed on a device yet.

> For setup, dependencies, and flashing instructions, see the [upstream README](https://github.com/android-a5y7lte/xnu-a5y17lte#readme). This fork only documents what it adds on top.

Bringing **mainline Linux (and later postmarketOS)** to the Samsung Galaxy A5 2017 (a5y17lte, Exynos 7880). The boot chain is:

Samsung S-Boot → mainline **U-Boot** (`a5y17lte_defconfig`) → **Linux** with a new `exynos7880-a5y17lte` device tree.

The repo name is historical. The earlier XNU/iOS investigation is closed, and the step 1 uniLoader hardware probe is kept for history. All confirmed hardware facts are in `EVIDENCE.md`.

## Status

- **Milestone 1: passed on the real phone (2026-10-18).**
  - Linux boots with all 8 cores and 2.6 GB of RAM.
  - The console shows on screen, and the panel refreshes by itself.
  - Reboot to TWRP works.
  - See `EVIDENCE.md` section 15.

- **Milestone 2 (keys): passed on the phone (2026-10-19).** All four keys work; holding Power reboots into TWRP.
- **Milestone 3 (eMMC, read-only): passed on the phone (2026-10-19).**
  - 8-bit DDR52 at 82 MiB/s; all 25 partitions read.
  - Needed kernel patch `0004`: the SMU must grant `SECURE_READ` on the Exynos7880.
  - Latest image: `out/a5y17lte-mainline-m3d-boot.img`.

- **Milestone 4 (USB networking): passed on the phone (2026-10-19).** USB NCM + ACM gadget after switching the S2MU004 MUIC to USB; PC gets `172.16.42.2` by DHCP; `ssh root@172.16.42.1` (blank password) and telnet work. `scp -O` works with M4e (latest): `out/a5y17lte-mainline-m4e-boot.img`.
  - Phone at `172.16.42.1`; `ssh root@172.16.42.1` (no password), `telnet 172.16.42.1`, or serial on `/dev/ttyACM0`.
  - eMMC still read-only. Hold Power for 3 s to reboot into TWRP.
- **Milestone 5a (distro kernel config, SYSTEM probe): passed on the phone (2026-10-19).** Every eMMC partition is now really read-only (EVIDENCE 19). SYSTEM is `mmcblk0p19`, 4848 MiB. Latest image: `out/a5y17lte-mainline-m5a-boot.img`. Builds are reproducible.

## GPU (implemented, untested on hardware)

The `exynos7880-gpu` branch adds a Mali-T830 MP3 node for the Exynos 7880. It is **not tested on hardware** — the maintainer's device is currently unavailable.

What this branch adds:

### Kernel — `patches/0001-exynos7880-gpu-clk-pd.patch`

- **`clk: samsung`**: add Exynos7880 `CMU_G3D` driver — adapted from `clk-exynos7870.c`, same register offsets per `S5E7880-cmusfr.h`.
- **`pmdomain: samsung`**: add Exynos7880 `pd_g3d` — `samsung,exynos7880-pd` compatible, `local_pwr_cfg = 0x7` *(placeholder, confirm in `S5E7880-pmusfr.h`)*.

### Device Tree — `mainline/dts/exynos7880.dtsi`

- **`gpu@11400000`**: `compatible = "arm,mali-t830", "arm,mali-midgard"`, IRQs `282`/`283`/`281` (job/mmu/gpu), clocks from `cmu_g3d` (`"core"`, `"bus"`), `power-domains = <&pd_g3d>`.
- **`cmu_g3d: clock-controller@11460000`**: `samsung,exynos7880-cmu-g3d`, parents `oscclk` and `g3d_switch` (fixed-clock at 399.75 MHz until `CMU_MIF` exists).
- **`pd_g3d: power-domain@106b4000`**: `samsung,exynos7880-pd`.

### Config — `mainline/config/a5y17lte.config`

- `CONFIG_DRM=y`, `CONFIG_DRM_PANFROST=y`, `CONFIG_DRM_GEM_SHMEM_HELPER=y`.
- `CONFIG_DEVFREQ_GOV_SIMPLE_ONDEMAND=y` — without it, Panfrost probe fails with error `-22`.

### postmarketOS — `pmos/` and `build-pmos.sh`

- **`device-samsung-a5y17lte/APKBUILD`**: added `firmware-samsung-universal7880` to `depends` (Wi-Fi). The `pmbootstrap` cache may need a pkgrel bump to actually install it.
- **`build-pmos.sh`**: builds the rootfs with the mainline kernel and the patches above.

## How to build

Same as upstream:

```bash
./setup.sh
./build-mainline.sh   # -> out/a5y17lte-mainline-mN-boot.img
./build-pmos.sh       # -> build/pmos/samsung-a5y17lte-root.img
```

The kernel patches are **not** applied by `setup.sh`; apply them manually:

```bash
cd upstream/linux
git am ../../patches/0001-exynos7880-gpu-clk-pd.patch
```

## What is missing for a working GPU

1. **Test on hardware** — `dmesg | grep -iE "panfrost|exynos7880-cmu|pd_g3d"`. If it fails, the likely culprits are `local_pwr_cfg` (`0x7`) and the `g3d_switch` fixed-clock.
2. **Mesa (userspace)** — compile with `-Dgallium-drivers=panfrost` and install it in the rootfs. The kernel alone does not render.
3. **`vdd_g3d` regulator** — the S2MPU05 driver is in Linux 6.15+, but the board DTS still needs the regulator node.
4. **Wi-Fi firmware** — `firmware-samsung-universal7880` is in `depends`, but the `pmbootstrap` cache may need a pkgrel bump to install it.

Everything else (display, keys, eMMC, USB networking, postmarketOS rootfs) is upstream and works as described in the original README.

## Layout

| Path | What it is |
|---|---|
| `EVIDENCE.md` | Every hardware/source claim, with file and line |
| `setup.sh` | Recreates all source trees at pinned revisions |
| `build-mainline.sh` | Builds the mainline kernel and packs the boot image |
| `build-pmos.sh` | Builds the postmarketOS rootfs with the mainline kernel |
| `patches/` | Kernel and uniLoader patches (applied manually for the GPU) |
| `mainline/dts/` | `exynos7880.dtsi`, `exynos7880-a5y17lte.dts` |
| `mainline/config/` | Kernel config fragments (incl. `a5y17lte.config`) |
| `mainline/init/` | nolibc init: info screen, panel refresh test, reboot to recovery |
| `mainline/a5y17lte.its`, `mainline/boot.cmd` | FIT payload and U-Boot boot script |
| `tools/emulate_uboot.py` | Runs the boot image on an emulated A53 up to the Linux entry |
| `tools/mkbootimg_v0.py` | Boot image packer/inspector (`pack` / `info`) |

## Credits

Fork of [`android-a5y7lte/xnu-a5y17lte`](https://github.com/android-a5y7lte/xnu-a5y17lte). Original mainline/uniLoader work by the upstream authors.

## License

Same as the upstream project.