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

echo "== Apple XNU (base for the port) =="
[ -d "$UP/x-xnu-8792.81.2" ] || git clone -q --depth 1 --branch xnu-8792.81.2 \
	https://github.com/apple-oss-distributions/xnu.git "$UP/x-xnu-8792.81.2"

echo "== Device kernel DTS/configs (reference only) =="
if [ ! -d "$UP/k7880/.git" ]; then
	git clone -q --depth 1 --branch hybris-17.1 --filter=blob:none --sparse \
		https://github.com/Exynos7880-Linux/android_kernel_samsung_universal7880.git "$UP/k7880"
	git -C "$UP/k7880" sparse-checkout set arch/arm64/boot/dts arch/arm64/configs drivers/video/fbdev/exynos
fi

echo "== Unicorn emulator (for tools/emulate_step1.py) =="
if [ ! -f "$UP/emu/unicorn/build/libunicorn.so" ]; then
	mkdir -p "$UP/emu"
	[ -d "$UP/emu/unicorn" ] || git clone -q --depth 1 --branch 2.1.4 \
		https://github.com/unicorn-engine/unicorn.git "$UP/emu/unicorn"
	cmake -S "$UP/emu/unicorn" -B "$UP/emu/unicorn/build" -DCMAKE_BUILD_TYPE=Release \
		-DUNICORN_ARCH=aarch64 -DUNICORN_BUILD_TESTS=OFF >/dev/null
	make -C "$UP/emu/unicorn/build" -j"$(nproc)" >/dev/null
fi
echo "done."
