#!/usr/bin/env bash
# Galaxy A5 (2017) mainline Linux (current milestone: M3c, eMMC SMU test).
# Builds: Linux 7.2 Image + A5 device tree, nolibc init + initramfs,
# U-Boot (a5y17lte_defconfig, position independent), a FIT payload, and the
# boot image S-Boot loads:  kernel = u-boot.bin, ramdisk = FIT.
# Everything is built with LLVM (clang, ld.lld, llvm-*).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LINUX="$ROOT/upstream/linux"
UBOOT="$ROOT/upstream/u-boot"
ML="$ROOT/mainline"
B="$ROOT/build"
OUT="$ROOT/out"
mkdir -p "$B/fit" "$OUT"
JOBS="$(nproc)"

for t in clang ld.lld llvm-objcopy flex bison python3 gzip; do
	command -v "$t" >/dev/null || { echo "missing tool: $t"; exit 1; }
done

# LLVM stand-ins for the aarch64-linux-gnu- prefix U-Boot expects
LC="$ROOT/upstream/tools/llvm-cross"
mkdir -p "$LC"
for t in ar nm objcopy objdump strip readelf ranlib; do ln -sf "$(command -v llvm-$t)" "$LC/aarch64-linux-gnu-$t"; done
ln -sf "$(command -v ld.lld)" "$LC/aarch64-linux-gnu-ld"
ln -sf "$(command -v ld.lld)" "$LC/aarch64-linux-gnu-ld.bfd"
export PATH="$LC:$PATH"

echo "== Linux: device tree + config + Image"
# Kernel driver changes live as patches (git am onto v7.2, branch a5y17lte);
# device trees are copied from mainline/dts so they stay easy to read.
if [ "$(git -C "$LINUX" rev-parse --abbrev-ref HEAD)" != a5y17lte ]; then
	git -C "$LINUX" checkout -q -B a5y17lte v7.2 2>/dev/null || git -C "$LINUX" checkout -q -B a5y17lte
	for p in "$ML"/patches/*.patch; do
		[ -e "$p" ] && git -C "$LINUX" -c user.name=a5y17lte -c user.email=a5y17lte@localhost am -q "$p"
	done
fi
cp "$ML"/dts/exynos7880*.dtsi "$ML"/dts/exynos7880-a5y17lte.dts "$LINUX/arch/arm64/boot/dts/exynos/"
grep -q exynos7880-a5y17lte "$LINUX/arch/arm64/boot/dts/exynos/Makefile" || \
	sed -i 's/^\(\texynos7870-on7xelte.dtb\t\t\\\)$/\1\n\texynos7880-a5y17lte.dtb\t\t\\/' \
		"$LINUX/arch/arm64/boot/dts/exynos/Makefile"
KM=(make -C "$LINUX" ARCH=arm64 LLVM=1)
KCONFIG_ALLCONFIG="$ML/config/a5y17lte.config" "${KM[@]}" allnoconfig >/dev/null
"${KM[@]}" -j"$JOBS" Image exynos/exynos7880-a5y17lte.dtb headers usr/gen_init_cpio >/dev/null
cp "$LINUX/arch/arm64/boot/Image" "$LINUX/arch/arm64/boot/dts/exynos/exynos7880-a5y17lte.dtb" "$B/fit/"

echo "== init (nolibc) + initramfs"
clang --target=aarch64-linux-gnu -Os -static -nostdlib -nostdinc \
	-isystem "$(clang -print-resource-dir)/include" \
	-fno-stack-protector -fno-asynchronous-unwind-tables -ffreestanding -fuse-ld=lld \
	-I "$LINUX/tools/include/nolibc" -I "$LINUX/usr/include" \
	-include "$LINUX/tools/include/nolibc/nolibc.h" -Wall -Werror \
	-o "$B/init" "$ML/init/init.c"
sed "s#build/init#$B/init#" "$ML/initramfs.list" > "$B/initramfs.list"
"$LINUX/usr/gen_init_cpio" "$B/initramfs.list" | gzip -9n > "$B/fit/initramfs.cpio.gz"

echo "== U-Boot (GNU toolchain: U-Boot's arm64 linker script needs GNU ld;
#    ld.lld places .bss at 0 - see EVIDENCE.md section 13)"
XP="$(ls -d "$ROOT"/upstream/tools/xpack/xpack-aarch64-none-elf-gcc-*/bin 2>/dev/null | head -1)"
[ -x "$XP/aarch64-none-elf-gcc" ] || { echo "missing xPack toolchain, run ./setup.sh"; exit 1; }
rm -rf "$B/u-boot"
UM=(make -C "$UBOOT" O="$B/u-boot" HOSTCC=clang CROSS_COMPILE="$XP/aarch64-none-elf-")
"${UM[@]}" a5y17lte_defconfig >/dev/null
"$UBOOT/scripts/config" --file "$B/u-boot/.config" \
	--enable POSITION_INDEPENDENT --disable TOOLS_MKEFICAPSULE
"${UM[@]}" olddefconfig >/dev/null
"${UM[@]}" -j"$JOBS" u-boot.bin tools >/dev/null

echo "== FIT payload"
cp "$ML/boot.cmd" "$ML/a5y17lte.its" "$B/fit/"
# mkimage runs "dtc" from PATH (compile-time MKIMAGE_DTC); use the one U-Boot built
ln -sf "$B/u-boot/scripts/dtc/dtc" "$LC/dtc"
( cd "$B/fit" && "$B/u-boot/tools/mkimage" -f a5y17lte.its "$B/a5y17lte.fit" >/dev/null )
"$B/u-boot/tools/mkimage" -l "$B/a5y17lte.fit"

echo "== boot image (docs/board/samsung/axy17lte.rst values)"
python3 "$ROOT/tools/mkbootimg_v0.py" pack \
	--kernel "$B/u-boot/u-boot.bin" --ramdisk "$B/a5y17lte.fit" \
	--base 0x40000000 --kernel_offset 0x00000000 --ramdisk_offset 0x01000000 \
	--tags_offset 0x00000100 --second_offset 0x00f00000 --pagesize 2048 \
	-o "$OUT/a5y17lte-mainline-m3c-boot.img"
python3 "$ROOT/tools/mkbootimg_v0.py" info "$OUT/a5y17lte-mainline-m3c-boot.img"
( cd "$OUT" && sha256sum a5y17lte-mainline-m3c-boot.img | tee a5y17lte-mainline-m3c-boot.img.sha256 )
