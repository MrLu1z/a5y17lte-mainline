#!/usr/bin/env bash
# Static aarch64 userspace for the test initramfs, built with clang + musl:
#   musl 1.2.5 (GitHub mirror kraj/musl, tag v1.2.5)
#   busybox 1.36.1 (GitHub mirror mirror/busybox, tag 1_36_1)
#   dropbear 2026.94 (github.com/mkj/dropbear, tag DROPBEAR_2026.94)
# libgcc.a (long double helpers) comes from the xPack toolchain set up by
# setup.sh. Output: upstream/userspace/out/{busybox,dropbearmulti}
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
U="$ROOT/upstream/userspace"
SYS="$U/sysroot"
LINUX="$ROOT/upstream/linux"
JOBS="$(nproc)"
mkdir -p "$U/bin" "$U/out"
# Reproducible: busybox's version banner takes its date from here.
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "$ROOT" log -1 --format=%ct)}"
export TZ=UTC LC_ALL=C

clone_at() {  # url dir tag
	[ -d "$2/.git" ] || git clone -q --depth 1 --branch "$3" "$1" "$2"
}
clone_at https://github.com/kraj/musl.git "$U/musl" v1.2.5
clone_at https://github.com/mirror/busybox.git "$U/busybox" 1_36_1
clone_at https://github.com/mkj/dropbear.git "$U/dropbear" DROPBEAR_2026.94

LIBGCC="$(ls "$ROOT"/upstream/tools/xpack/*/lib/gcc/aarch64-none-elf/*/libgcc.a | head -1)"
[ -f "$LIBGCC" ] || { echo "run ./setup.sh first (xPack toolchain)"; exit 1; }

echo "== musl"
if [ ! -f "$SYS/lib/libc.a" ]; then
	( cd "$U/musl" &&
	  CC="clang --target=aarch64-linux-musl" AR=llvm-ar RANLIB=llvm-ranlib \
	  LIBCC="$LIBGCC" ./configure --prefix="$SYS" --target=aarch64-linux-musl \
	      --disable-shared >/dev/null &&
	  make -j"$JOBS" >/dev/null && make install >/dev/null )
fi
make -s -C "$LINUX" ARCH=arm64 LLVM=1 headers
cp -r "$LINUX/usr/include/." "$SYS/include/"
find "$SYS/include" -name '.*' -delete
cp "$LIBGCC" "$SYS/lib/libgcc.a"
for o in crtbeginT crtbegin crtend; do	# musl uses init_array; empty is fine
	echo '' | clang --target=aarch64-linux-musl -x c -c - -o "$SYS/lib/$o.o"
done
echo 'int __libgcc_eh_placeholder;' | clang --target=aarch64-linux-musl -x c -c - -o "$U/eh.o"
llvm-ar rc "$SYS/lib/libgcc_eh.a" "$U/eh.o"

cat > "$U/bin/aarch64-musl-cc" <<EOF
#!/bin/sh
# clang for static aarch64 Linux binaries against the local musl sysroot.
exec clang --target=aarch64-linux-musl --sysroot=$SYS -nostdinc \\
  -isystem $SYS/include -isystem "\$(clang -print-resource-dir)/include" \\
  -fuse-ld=lld -static -B$SYS/lib -L$SYS/lib --rtlib=libgcc --unwindlib=none \\
  -Wno-unused-command-line-argument "\$@"
EOF
chmod +x "$U/bin/aarch64-musl-cc"
CC="$U/bin/aarch64-musl-cc"

echo "== busybox"
( cd "$U/busybox" &&
  make -s defconfig >/dev/null &&
  sed -i 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/;
          s/^CONFIG_TC=y/# CONFIG_TC is not set/;
          s/^CONFIG_FEATURE_TC_INGRESS=y/# CONFIG_FEATURE_TC_INGRESS is not set/' .config &&
  make -s oldconfig </dev/null >/dev/null &&
  make -j"$JOBS" CC="$CC" HOSTCC=gcc AR=llvm-ar STRIP=llvm-strip busybox >/dev/null )
cp "$U/busybox/busybox" "$U/out/busybox"

echo "== dropbear"
( cd "$U/dropbear" &&
  cat > localoptions.h <<'EOF'
/* Galaxy A5 test image: dropbear over USB networking only. */
#define DROPBEAR_SVR_PASSWORD_AUTH 1
#define DEFAULT_PATH "/bin:/sbin"
EOF
  [ -f Makefile ] || CC="$CC" AR=llvm-ar RANLIB=llvm-ranlib ./configure \
      --host=aarch64-linux-musl --disable-zlib --enable-static \
      --disable-lastlog --disable-utmp --disable-utmpx --disable-wtmp \
      --disable-wtmpx --disable-pututline --disable-pututxline \
      --disable-syslog >/dev/null &&
  make -j"$JOBS" PROGRAMS="dropbear dropbearkey scp" MULTI=1 STATIC=1 >/dev/null )
llvm-strip -o "$U/out/dropbearmulti" "$U/dropbear/dropbearmulti"

ls -la "$U/out"
