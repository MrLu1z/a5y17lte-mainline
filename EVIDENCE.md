# Galaxy A5 2017 (a5y17lte, Exynos 7880): evidence log

> **Direction change (2026-10-18):** the XNU/iOS investigation (sections 1–8) is closed. The project now targets **mainline Linux / postmarketOS** booted by mainline U-Boot (sections 13+). The hardware facts in sections 9–12 apply unchanged.

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

## 11. Hardware result 1 (phone, 2026-10-18 02:27)

**Result.** After flashing `a5y17lte-step1-boot.img`, the Samsung "Galaxy A5 / powered by android" logo stayed on screen for more than 2 minutes. There was no uniLoader text.

**Explanation.**

- The panel is **MIPI command mode with hardware TE trigger** (`exynos7880.dtsi` decon_f: `psr_mode = <2>`, `trig_mode = <0>`).
- In command mode the panel keeps its own frame memory and only updates when DECON is triggered (`decon_reg_7880.c:611` `decon_reg_set_trigger`).
- Step 1 wrote `0x1281` once, before drawing, and never triggered again. So any drawing could not reach the panel.

**Inconclusive.** This does not show whether the code ran.

**Fix (step 1b):**

- DECON is put in software trigger, with `TRIG_AUTO_MASK` and one `SW_TRIG` frame per printk line.
- Then the board reboots via PMU `SWRESET` (`0x106B0400` = 1, from `S5E7880-pmusfr.h:161` and `exynos-reboot.c:72,299`) after 30 s.
- A self-reboot proves execution even with no picture.

**Emulated:** 333 frames fired, all text drawn, the reset write reached, and no other MMIO touched.

## 12. Hardware result 2: step 1b WORKS on the phone (2026-10-18)

Photo: `evidence/hw-step1b-2026-10-18.jpg`. uniLoader built 02:28:46 (commit 79cdfbe) ran on the real a5y17lte. All text and the R G B W bars are visible.

**Display and loader**

| Reading | Meaning |
|---|---|
| Text visible on screen | S-Boot runs a raw uniLoader from the boot partition. The command-mode trigger fix works. |
| Bars appear in R G B W order | The framebuffer is `0x67000000`, 1080×1920, ARGB8888. **Confirmed.** |
| DECON TRIG = `0x1000` | The SW_TRIG bit self-clears after a frame. |
| WIN0 = `0xffff0121` (enabled); WIN1-3 = `0x300` (off) | Only window 0 is in use. |
| IDMA1 (`0x14822000`) ADDR_Y0 = `0x67000000`; IDMA0 = 0 | S-Boot scans out through **IDMA G1**, not G0. |

**CPU state at hand-off**

| Register | Value | Meaning |
|---|---|---|
| CurrentEL | **EL1** | We are handed control at EL1, the level XNU's `start.s` expects. EL2 is not available to us. |
| MIDR | `0x410fd034` | Cortex-A53 r0p4. |
| MPIDR | `0x80000000` | Boot CPU is cluster 0, core 0. |
| **CNTFRQ_EL0** | **0** | Firmware never set it, and EL1 cannot. XNU must take the 26 MHz timer from the device tree (`timebase-frequency`, `pexpert/arm/pe_identify_machine.c:85`), never from CNTFRQ. |
| DAIF | `0x2c0` | D, I and F masked; SError unmasked. |
| SCTLR_EL1 | `0x30c5183c` | MMU off (M=0); C and I bits set. Leftover cache state from S-Boot means XNU's entry must clean/invalidate before enabling the MMU. |
| TCR_EL1 | `0x0000002000000020` | Leftover from S-Boot; ignored because the MMU is off. |
| VBAR_EL1 | `0x43e00800` | S-Boot's vectors are at 0x43e00000. XNU and its device tree must not be placed over that region before XNU installs its own vectors. |
| ID_AA64PFR0 | `0x2222` | EL0-3 implemented. GIC field = 0, so no GICv3 system registers. **This confirms a memory-mapped GICv2 only** (section 3). |
| ID_AA64MMFR0 | `0x1122` | 40-bit physical addresses, 16-bit ASIDs. **TGran16 = 0, so the 16 KB page granule is unsupported**; only 4 KB and 64 KB work. |

**Page size check in XNU 8792**

- 16 KB pages come from `__ARM_16K_PG__`, defined only in `H13.h:73` and `VMAPPLE.h:44`.
- `BCM2837.h` does not define it, so the A53 target uses `ARM_PGSHIFT 12` (4 KB, `proc_reg.h:1149-1152`). This matches the hardware.
- The iOS 7.1.2 (A7) kernel also uses 4 KB pages.

**Reboot (confirmed by Jimmy, 11:37)**

- The phone reboots by itself about every 30 s and loops back into step 1b.
- This shows that PMU `SWRESET` (`0x106B0400` = 1) works from EL1.
- It also shows that the physical counter runs at about 26 MHz, so the 26 MHz timer from the device tree is correct.

## 13. Mainline Linux, milestone 1: sources and decisions (2026-10-18)

**Base versions:** Linux **v7.2** (8d3ae5928, 2026-08-16), U-Boot **v2026.10** (55084065).

**What mainline already has.** In v7.2 the 7870 sibling has:

- `exynos7870.dtsi`, `clk-exynos7870.c` and 7870 pinctrl
- five phones: a2corelte, j5y17lte, j6lte, j7xelte, on7xelte

There is no 7880 support yet.

**7880 compared with 7870.** Taken from the Samsung `pwrcal S5E78x0-sfrbase.h` files and the DTS.

| Block | Status |
|---|---|
| CMU PERI, FSYS, G3D, DISPAUD, ISP, MFCMSCL, CPUCL0/1 | Same addresses |
| UARTs, MMC, USB, I2C, DECON, pinctrl ALIVE/FSYS/TOUCH | Same addresses |
| CMU_MIF | 7870 has one at 0x10460000; 7880 has CMU_MIF0 at 0x10450000, CMU_MIF1 at 0x10550000 and CMU_CCORE at 0x10680000 |
| GIC | 0x104E1000 on 7870 vs **0x106E1000** on 7880 |
| PMU | 0x10480000 on 7870 vs **0x106B0000** on 7880 |
| MIF pinctrl | 0x10530000 on 7870 vs 0x10630000 on 7880 |

A full port needs a 7880 clock-driver variant. Milestone 1 needs no clocks.

**Cross-checks against independent sources:**

- **CNTFRQ.** Mainline `exynos7870.dtsi` hard-codes `clock-frequency = <26000000>` with the comment "Non-updatable, broken stock Samsung bootloader does not configure CNTFRQ_EL0". This matches our hardware reading CNTFRQ_EL0 = 0 (section 12).
- **Reboot modes.** On the 7880, Samsung `sec_ext.h` puts INFORM3 at PMU+0x80C, and `sec_reboot.c` uses prefix 0x12345670 with the codes download=1, recovery=4, bootloader=0xd. These are identical to the mainline 7870 `syscon-reboot-mode` values.

**Display.** The panel is MIPI command mode (`psr_mode = <2>`, `trig_mode = <0>`).

- Samsung `decon_reg_configure_trigger` (decon_reg_7880.c:384) sets HW_TRIG_EN, and `decon_reg_set_trigger` unmasks HW_TRIG_MASK.
- So the init writes HW_SW_TRIG_CONTROL = 0x10 (HW trigger, unmasked, no auto-mask). This is a **hypothesis to be confirmed on the phone**.
- The fallback is step 1b's software trigger (0x1000 then 0x1100) through /dev/mem.

**Memory.** Taken from Samsung `exynos7880-rmem.dtsi`.

- Bank 0 is 0x40000000 + 0x3E400000; bank 1 is 0x80000000 + 0x80000000.
- 0x7E400000–0x80000000 is secure DRAM.
- M1 marks every Samsung carve-out no-map, plus 0x67000000–0x7E400000 (framebuffer, modem, gnss and ION).

**U-Boot facts (v2026.10):**

- **DRAM banks.** `board/samsung/common/board.c` sizes DRAM as 12×256 MB from 0x40000000 using `get_ram_size()`.
- **Memory fixup.** U-Boot rewrites the kernel's `/memory` node to those banks, **including the secure hole**. M1's DTS therefore lists `secure-dram@7e400000` as no-map explicitly. This was confirmed in emulation: the fixed-up `/memory` has 12 contiguous banks, and every no-map node survives.
- **Boot command.** `bootcmd=source $prevbl_initrd_start_addr:bootscript` (include/configs/exynos78x0-common.h). S-Boot's ramdisk carries a FIT that contains the boot script.
- **Load address.** `a5y17lte_defconfig` is linked at 0x40001000 and is **not** position-independent. S-Boot's real load address is unknown, so M1 enables `CONFIG_POSITION_INDEPENDENT` ("almost any 4K aligned address").
- **Toolchain.**
  - **ld.lld cannot link U-Boot's arm64 image.** `arch/arm/cpu/armv8/u-boot.lds` places `.bss ADDR(.rela.dyn) (OVERLAY)`. ld.lld put `__bss_start` at 0x0, so U-Boot computed `Monitor len: c00086d0`, reserved about 3 GB, and wrote below RAM. This was found in emulation with a DEBUG build.
  - **GNU ld fixes it.** The xPack aarch64-none-elf GCC 14.2.1-1.1 (ld 2.43.1, sha256 a2c9731f…b117, matching the release .sha) gives `__bss_start` 0x40067a60 and `__bss_end` 0x40071120.

## 14. Milestone 1 build and emulation (2026-10-18)

**Image.** `out/a5y17lte-mainline-m1-boot.img` contains:

- **kernel:** u-boot.bin (491 KB)
- **ramdisk:** FIT (5.8 MB) holding the Linux 7.2 Image (6,078,472 B, sha256 33ebee32…), the DTB, the initramfs (nolibc init) and the boot script

**Emulation** (`tools/emulate_uboot.py`, log in `out/emulated-uboot-m1.log`):

- U-Boot was started at 0x40080000, 0x40001000 and 0x40000000, with the FIT at 0x41000000 and at 0x5A000000.
- In every combination U-Boot:
  - printed `DRAM: 3 GiB`
  - ran the FIT boot script and verified all sha256 hashes
  - loaded the kernel at 0x48000000 (`ARMd` magic), the initrd at 0x4FFFF000 and the FDT at 0x4FFFB000
  - jumped to Linux with x0 = the FDT

**Limit.** Unicorn cannot follow Linux past `__enable_mmu`: it raises an unhandled prefetch abort (exception type 3). Linux itself is therefore unverified until the phone test.

**What the phone test shows:**

- Text appearing before t = 15 means the TE hardware refresh works.
- Text appearing only at t = 15 means the software refresh works and TE does not.
- The phone landing in TWRP at t = 180 means the reboot-mode node works.
- If the screen shows nothing and the phone never reboots, Linux did not reach init.

## Open items

1. **Phone test of milestone 1:**
   - Does Linux reach init?
   - Does the TE refresh work?
   - Does reboot-to-recovery work?
2. **TWRP readings** (`/proc/iomem`, `/proc/cmdline`, `/proc/device-tree`, `chosen/linux,initrd-start`):
   - where S-Boot really loads the ramdisk (FIT), to confirm it cannot overlap the kernel at 0x48000000
   - the real memory map
3. **Shrinking the carve-outs:** decide which M1 no-map regions are truly secure and which can return to Linux.
4. **Next milestones:**
   - 7880 clock driver (MIF0/MIF1/CCORE)
   - pinctrl, then gpio-keys
   - UART
   - MMC (eMMC/SD)
   - USB gadget networking
   - touch
   - DSI/DECON (Linux 6.18+ has the 7870 DSIM)
   - postmarketOS device package
5. ~~Framebuffer address~~ confirmed in section 12. ~~UART cable~~ not needed; the screen is the console.
