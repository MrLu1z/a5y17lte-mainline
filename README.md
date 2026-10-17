# xnu-a5y17lte

Porting open-source XNU to the Samsung Galaxy A5 (2017), a5y17lte (Exynos 7880), using uniLoader as the bootloader.

This project is separate from the Sailfish work (`~/hadk`, `~/sfos`) and never touches it.

## Layout

| Path | What it is |
|---|---|
| `EVIDENCE.md` | Every hardware/source claim, with file and line |
| `setup.sh` | Recreates all source trees at pinned revisions |
| `build-step1.sh` | Builds uniLoader and packs `out/a5y17lte-step1b-boot.img` |
| `patches/` | The a5y17lte uniLoader changes (applied by `setup.sh`) |
| `tools/mkbootimg_v0.py` | Boot image packer/inspector (`pack` / `info`) |
| `tools/emulate_step1.py`, `tools/run-emulator.sh` | Runs the boot image on an emulated Cortex-A53 and saves the screen as PNG |
| `uniLoader/` | Fork, branch `a5y17lte` (created by `setup.sh`) |
| `upstream/` | Pristine uniLoader, XNU 8792.81.2, device DTS, Unicorn |

## How the pieces connect

```
setup.sh ──► upstream/uniLoader @bdc0f6d ──(+ patches/*.patch)──► uniLoader/ (branch a5y17lte)
build-step1.sh ──► make a5y17lte_defconfig ──► uniLoader/uniLoader (raw binary)
               └─► tools/mkbootimg_v0.py pack ──► out/a5y17lte-step1b-boot.img (+ .sha256)
tools/run-emulator.sh ──► loads that same boot.img ──► out/step1-screen.png
```

Inside the uniLoader patch:

```
configs/a5y17lte_defconfig ─► CONFIG_EXYNOS_7880 ─► soc/Kconfig ─► soc/Makefile ─► soc/exynos/exynos7880.c
                           └► CONFIG_SAMSUNG_A5Y17LTE ─► board/Kconfig ─► board/Makefile ─► board/samsung/board-a5y17lte.c
                                                                                             └► include/soc/exynos7880.h
```

## Build on your PC (Linux Mint)

```bash
sudo apt install git clang lld llvm cmake build-essential python3
./setup.sh
./build-step1.sh
tools/run-emulator.sh        # optional: emulated screen at out/step1-screen.png
```

## Step 1: flash and test (TWRP)

1. **Back up the current boot partition.** In TWRP, go to Backup, tick **Boot** only, and swipe. This holds your Sailfish boot image.
2. Copy `a5y17lte-step1b-boot.img` to the phone.
3. In TWRP, go to Install, tap **Install Image**, pick the file, and choose **Boot**.
4. Reboot to System.
5. **Photograph the screen** and send the photo back.
6. To get back:
   - Hold **Vol Down + Power** about 7 seconds.
   - The moment the screen goes black, switch to **Vol Up + Home + Power** to enter TWRP.
   - Go to Restore, pick your boot backup, and restore it.

Only the boot partition changes, and the backup restores it exactly.

### Reading the result

| What you see | Meaning |
|---|---|
| Text and **R G B W** bars in that order | uniLoader runs, framebuffer is 0x67000000, and the format is ARGB8888. Read off `CurrentEL` and the IDMA lines. |
| Bars in a different colour order | Wrong pixel format. Tell me the order. |
| Text stops after `reading IDMAn...` | That display block is clock-gated. Still useful; send the photo. |
| Logo stays, phone **reboots by itself after ~30-40 s** | Code ran; display trigger still wrong. |
| Logo stays forever, never reboots | S-Boot did not run it or the framebuffer address is wrong. Next try: framebuffer auto-detect build. |
| "Kernel is not seandroid enforcing" in red at top | Normal warning, ignore it. |

## Status

- Step 1 builds and passes emulation at load addresses 0x40000000, 0x40080000 and 0x40001000.
- The screen is byte-identical in all three runs.
- Not yet run on the phone.
