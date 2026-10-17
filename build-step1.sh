#!/usr/bin/env bash
# Step 1: build uniLoader for a5y17lte (diagnostics + halt) and pack a
# flashable boot image. Run from anywhere; everything is relative to this file.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UL="$ROOT/uniLoader"
OUT="$ROOT/out"
mkdir -p "$OUT"

# Toolchain: LLVM (clang + ld.lld + llvm-objcopy). On Linux Mint:
#   sudo apt install clang lld llvm
for t in clang ld.lld llvm-objcopy python3; do
	command -v "$t" >/dev/null || { echo "missing tool: $t"; exit 1; }
done

# uniLoader always embeds kernel/dtb/ramdisk blobs. Step 1 halts before
# using them, so placeholders are enough.
for f in Image dtb ramdisk; do
	[ -e "$UL/blob/$f" ] || printf 'A5Y17LTE-STEP1-PLACEHOLDER\0' > "$UL/blob/$f"
done

make -C "$UL" ARCH=aarch64 LLVM=1 a5y17lte_defconfig
make -C "$UL" ARCH=aarch64 LLVM=1 -j"$(nproc)"

# Tiny valid ramdisk (gzipped newc cpio containing only the TRAILER),
# in case S-Boot refuses an empty ramdisk.
python3 - "$OUT/ramdisk-empty.cpio.gz" <<'EOF'
import gzip, sys
name = b"TRAILER!!!\0"
hdr = b"070701" + b"".join(b"%08X" % v for v in
      (0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, len(name), 0))
body = hdr + name
body += b"\0" * ((-len(body)) % 4)
body += b"\0" * ((-len(body)) % 512)
open(sys.argv[1], "wb").write(gzip.compress(body, mtime=0))
EOF

# mkbootimg values from docs.u-boot.org board/samsung/axy17lte
# (S-Boot ignores the offsets, uniLoader relocates itself to 0x47000000).
python3 "$ROOT/tools/mkbootimg_v0.py" pack \
	--kernel "$UL/uniLoader" \
	--ramdisk "$OUT/ramdisk-empty.cpio.gz" \
	--base 0x40000000 --kernel_offset 0x00000000 \
	--ramdisk_offset 0x01000000 --tags_offset 0x00000100 \
	--second_offset 0x00f00000 --pagesize 2048 \
	-o "$OUT/a5y17lte-step1-boot.img"

python3 "$ROOT/tools/mkbootimg_v0.py" info "$OUT/a5y17lte-step1-boot.img"
( cd "$OUT" && sha256sum a5y17lte-step1-boot.img > a5y17lte-step1-boot.img.sha256 && cat a5y17lte-step1-boot.img.sha256 )
