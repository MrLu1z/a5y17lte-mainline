#!/usr/bin/env bash
# Recreate every source tree this project depends on, pinned to the exact
# revisions the evidence log was checked against. Safe to re-run.
# Keeps everything inside this folder; never touches ~/hadk or ~/sfos.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UP="$ROOT/upstream"
mkdir -p "$UP"

clone_at() {  # url dir ref
	[ -d "$2/.git" ] || git clone -q "$1" "$2"
	git -C "$2" fetch -q --depth 1 origin "$3" 2>/dev/null || git -C "$2" fetch -q origin
	git -C "$2" checkout -q "$3"
	echo "  $2 @ $(git -C "$2" log -1 --format=%h)"
}

# ---- legacy: step 1 hardware probe (uniLoader). Kept for history only; no
# further uniLoader work (its AGENTS.md asks AI agents not to work on it).
echo "== uniLoader (upstream, pristine) =="
clone_at https://github.com/ivoszbg/uniLoader.git "$UP/uniLoader" bdc0f6d

echo "== uniLoader (a5y17lte fork = upstream + patches/) =="
if [ ! -d "$ROOT/uniLoader/.git" ]; then
	git clone -q "$UP/uniLoader" "$ROOT/uniLoader"
	git -C "$ROOT/uniLoader" checkout -q -b a5y17lte bdc0f6d
	git -C "$ROOT/uniLoader" -c user.name=a5y17lte -c user.email=a5y17lte@localhost \
		am -q "$ROOT"/patches/*.patch
fi
echo "  $ROOT/uniLoader @ $(git -C "$ROOT/uniLoader" log -1 --format=%h)"

echo "== Linux v7.2 (mainline) =="
[ -d "$UP/linux/.git" ] || git clone -q --depth 1 --branch v7.2 \
	https://github.com/torvalds/linux.git "$UP/linux"

echo "== U-Boot v2026.10 (has a5y17lte_defconfig) =="
[ -d "$UP/u-boot/.git" ] || git clone -q --depth 1 --branch v2026.10 \
	https://github.com/u-boot/u-boot.git "$UP/u-boot"

echo "== xPack GNU AArch64 toolchain 14.2.1-1.1 (U-Boot needs GNU ld) =="
XV=14.2.1-1.1
XF=xpack-aarch64-none-elf-gcc-$XV-linux-x64.tar.gz
if [ ! -d "$UP/tools/xpack/xpack-aarch64-none-elf-gcc-$XV" ]; then
	mkdir -p "$UP/tools/dl" "$UP/tools/xpack"
	( cd "$UP/tools/dl" &&
	  curl -sSLf -o "$XF" "https://github.com/xpack-dev-tools/aarch64-none-elf-gcc-xpack/releases/download/v$XV/$XF" &&
	  echo "a2c9731f3c2a0abfdfa1e285de82d806e07a580a5fef4be59aac952c6b67b117  $XF" | sha256sum -c - )
	tar xzf "$UP/tools/dl/$XF" -C "$UP/tools/xpack"
fi

echo "== flex (kernel/U-Boot host tool), if missing =="
if ! command -v flex >/dev/null; then
	mkdir -p "$UP/tools/dl"
	( cd "$UP/tools/dl" &&
	  curl -sSLf -o flex-2.6.4.tar.gz https://github.com/westes/flex/releases/download/v2.6.4/flex-2.6.4.tar.gz &&
	  echo "e87aae032bf07c26f85ac0ed3250998c37621d95f8bd748b31f15b33c45ee995  flex-2.6.4.tar.gz" | sha256sum -c - &&
	  tar xzf flex-2.6.4.tar.gz && cd flex-2.6.4 && ./configure -q && make -j"$(nproc)" >/dev/null &&
	  sudo make install >/dev/null )
fi

echo "== Device kernel DTS/configs (reference only) =="
if [ ! -d "$UP/k7880/.git" ]; then
	git clone -q --depth 1 --branch hybris-17.1 --filter=blob:none --sparse \
		https://github.com/Exynos7880-Linux/android_kernel_samsung_universal7880.git "$UP/k7880"
	git -C "$UP/k7880" sparse-checkout set arch/arm64/boot/dts arch/arm64/configs \
		drivers/video/fbdev/exynos drivers/soc/samsung drivers/staging/samsung include/linux
fi

echo "== Unicorn emulator (tools/emulate_step1.py, tools/emulate_uboot.py) =="
if [ ! -f "$UP/emu/unicorn/build/libunicorn.so" ]; then
	mkdir -p "$UP/emu"
	[ -d "$UP/emu/unicorn" ] || git clone -q --depth 1 --branch 2.1.4 \
		https://github.com/unicorn-engine/unicorn.git "$UP/emu/unicorn"
	cmake -S "$UP/emu/unicorn" -B "$UP/emu/unicorn/build" -DCMAKE_BUILD_TYPE=Release \
		-DUNICORN_ARCH=aarch64 -DUNICORN_BUILD_TESTS=OFF >/dev/null
	make -C "$UP/emu/unicorn/build" -j"$(nproc)" >/dev/null
fi
echo "done."
