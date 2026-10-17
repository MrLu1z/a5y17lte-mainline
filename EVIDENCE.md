# XNU on Galaxy A5 2017 (a5y17lte, Exynos 7880) — Evidence Log

Every claim below was checked against source on 2026-10-18. Sources are in `upstream/`.

| Source | Version |
|---|---|
| Apple XNU | `apple-oss-distributions/xnu`, tags xnu-4570.1.46, xnu-6153.141.1, xnu-8792.81.2, xnu-12377.121.6 |
| UniLoader | `ivoszbg/uniLoader` @ bdc0f6d (2026-10-13) |
| Device kernel | `Exynos7880-Linux/android_kernel_samsung_universal7880`, branch hybris-17.1 (DTS + configs only) |

## 1. XNU has a Cortex-A53 target (BCM2837 / Raspberry Pi 3)

| Tag | Files mentioning BCM2837 |
|---|---|
| xnu-4570.1.46 (first public ARM64) | 0 |
| xnu-6153.141.1 | 11 |
| xnu-8792.81.2 | 10 |
| xnu-12377.121.6 (latest) | 0 — removed |

In xnu-8792.81.2:

- `makedefs/MakeInc.def:32` sets `SUPPORTED_ARM64_MACHINE_CONFIGS = BCM2837 T6000 T8101 T8103 VMAPPLE`.
- `pexpert/pexpert/arm64/board_config.h:90` defines a BCM2837 board: 4 CPUs, 1 cluster, KPC off.
- `pexpert/arm/pe_serial.c:493` contains the `PI3_UART` mini-UART driver.
- `osfmk/arm64/start.s:689` routes the BCM2837 timer interrupt to FIQ before the MMU is turned on.
- `#ifndef BCM2837` skips lines exist in `osfmk/kern/startup.c:729` and `iokit/Kernel/IOStartIOKit.cpp:81`.

**Conclusion:** use xnu-8792.81.2 as the base. It is the newest release that still has the A53 board path.

## 2. XNU already speaks the Samsung UART

`pe_serial.c` (`APPLE_UART`) programs `rULCON0` and `rUTRSTAT0`, the Samsung s3c/exynos UART register layout. It binds to the device-tree compatible string `"uart-1,samsung"` (line 877) when `enable-legacy-serial` is set.

The 7880 has two UARTs, both `samsung,exynos-uart`:

- `uart@13810000` (exynos7880.dtsi:335)
- `uart@13820000` (exynos7880.dtsi:348)

**Conclusion:** the serial driver should mostly work as-is once the device tree points at 0x13820000. This still needs proving on hardware.

## 3. The interrupt controller does NOT match — main porting work

- **Exynos 7880:** GICv2 (`arm,cortex-a15-gic`) at `0x106E1000` (distributor) and `0x106E2000` (CPU interface). Source: exynos7880.dtsi:270.
- **XNU:** the only GIC code is GICv3, under `HAS_GIC_V3` for VMAPPLE (`pexpert/arm/pe_fiq.c:15`, `VMAPPLE.h:40`). It uses redistributors, which GICv2 does not have.
- **XNU interrupt-controller drivers:** only abstract classes and examples ship (`IOInterruptController.h`, `PassthruInterruptController.h`, `Examples/drvGenericInterruptController`). Apple's real AIC driver is closed source.

**Required work:**

- Write a GICv2 backend for `pe_fiq.c`, which handles timer FIQs.
- Write a GICv2 `IOInterruptController` for device IRQs.
- **Risk:** Samsung's EL3 monitor may own GIC Group 0 (FIQ). If so, the timer must go through IRQ instead. Verify on hardware.

## 4. Timer, CPUs, power

- **Timer:** `arm,armv8-timer` at 26 MHz, using PPIs 13/14/11/10. XNU arm64 already uses the generic timer. Set `timebase-frequency` = 26000000 in the Apple device tree.
- **CPUs:** 8× `arm,cortex-a53` in two clusters (cpu@0..3 and cpu@100..), started via `enable-method = "psci"` with `method = "smc"`. BCM2837 is 4 CPUs in 1 cluster, so for the first boot use one CPU only. SMP later needs PSCI CPU_ON.

## 5. Memory and display

- **RAM:** `0x40000000` size `0x3E400000`, plus `0x80000000` size `0x80000000` (a5y17lte_common.dtsi:21,26). The gap at 0x7E400000 is reserved by firmware.
- **Display:** DECON at `0x14830000` (exynos7880.dtsi:1406), the same base UniLoader already uses for a3xelte (Exynos 7580). The A5 framebuffer is 1080×1920. Its physical address is unknown; read it from DECON window registers at runtime or from the device.

## 6. UniLoader

- Boots Linux only. A search for xnu, mach-o, boot_args and darwin found nothing.
- Supported Exynos SoCs: 7420, 7570, 7885, 8895, 9610, 9810, 990. There is no 7880 SoC or board.
- `board/samsung/board-a3xelte.c` is a close template: same DECON base and the same simplefb approach.

**Required work:**

- Add an Exynos 7880 SoC and an a5y17lte board.
- Add an XNU payload path: load the Mach-O, build `boot_args` (with Video = framebuffer), build an Apple-format flattened device tree from the facts above, then jump.

## 7. Samsung firmware

`hybris-a5y17lte_defconfig` has `CONFIG_TIMA_LKMAUTH=y` but no `CONFIG_UH*` or `CONFIG_RKP*`. That suggests no EL2 hypervisor sits in the way of a non-Linux kernel. Unconfirmed until tested on hardware.

## 8. iOS 7.1.2 kernelcache (experiment track)

- Not verified yet. That needs the iPhone 5s IPSW and reverse engineering.
- The expected blockers, still unconfirmed:
  - an Apple-format device tree from iBoot,
  - AIC-only interrupt handling inside closed kexts,
  - Cyclone implementation-defined system registers, which trap on a Cortex-A53.

## 9. Boot image and framebuffer facts used in step 1

- **Boot image packaging.** Mainline U-Boot packages for a5y17lte with:
  - `mkbootimg --base 0x40000000 --kernel_offset 0 --ramdisk_offset 0x01000000 --tags_offset 0x100 --pagesize 2048 --second_offset 0xf00000`
  - no dt.img
  - its docs say stock S-Boot ignores the offsets (docs.u-boot.org, board/samsung/axy17lte)
- **UART.** U-Boot's `exynos78x0.dtsi` uses UART2 at `0x13820000` (`samsung,exynos4210-uart`), matching section 2.
- **Framebuffer at `0x67000000`.** Every sibling Exynos S-Boot of this generation uses it:
  - uniLoader `board-j4lte.c` (7570) and `board-a3xelte.c` (7580)
  - mainline `exynos7870-j5y17lte.dts` (2026-03 patch)
  - U-Boot `exynos7870-on7xelte` (1080×1920, same as the A5)

  The 7870 J5 also has the identical DRAM bank `0x40000000 + 0x3e400000`.

  **Not yet confirmed on the A5.** The step 1 image prints the IDMA base-address registers to settle it.
- **DECON trigger.** `HW_SW_TRIG_CONTROL` is at offset `0x70` (`decon_7880/regs-decon.h:92`), the same as 7570/7885, so the same `0x1281` write is used.

## 10. Step 1 results (emulated, 2026-10-18)

- **Test setup.** The real `out/a5y17lte-step1-boot.img` runs in Unicorn 2.1.4 (QEMU CPU core, Cortex-A53 model).
- **Results:**
  - The image self-relocates to `0x47000000` and halts in `wfe` after 35.0M instructions.
  - Tested at load addresses `0x40000000`, `0x40080000` and `0x40001000`. The framebuffer is byte-identical in all three.
- **MMIO touched:** only DECON `0x14830070` (write `0x1281`), DECON WIN0-3, and IDMA0-5 `+0x74`. No other hardware.
- **Not proven by emulation:** real S-Boot entry state, real register values, and real scan-out. That is the phone test.

## Open items

1. A UART cable for the A5 (USB-C debug UART), or framebuffer-only console.
2. The framebuffer physical address.
3. Whether GIC Group 0 / FIQ is usable from EL1 under Samsung's EL3.
4. A build toolchain: XNU is normally built with Apple's toolchain, so a Linux build needs community tooling. Not evaluated yet.
