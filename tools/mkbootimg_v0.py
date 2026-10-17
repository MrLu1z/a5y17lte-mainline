#!/usr/bin/env python3
"""Minimal Android boot image v0 packer/inspector for Samsung S-Boot (a5y17lte).

Header layout follows AOSP system/tools/mkbootimg (boot_img_hdr_v0).
On Samsung boot images the header_version slot is used as dt_size;
we leave it 0 (no dt.img), which is how mainline U-Boot is packaged
for a5y17lte (docs.u-boot.org board/samsung/axy17lte).

Usage:
  mkbootimg_v0.py pack --kernel K [--ramdisk R] -o boot.img [options]
  mkbootimg_v0.py info boot.img
"""
import argparse
import hashlib
import struct
import sys

MAGIC = b"ANDROID!"
HDR_FMT = "<8s10I16s512s32s1024s"
HDR_SIZE = struct.calcsize(HDR_FMT)  # 1632
SEANDROID = b"SEANDROIDENFORCE"


def pad(data, page):
    return data + b"\0" * ((-len(data)) % page)


def pack(a):
    kernel = open(a.kernel, "rb").read()
    ramdisk = open(a.ramdisk, "rb").read() if a.ramdisk else b""
    page = a.pagesize

    sha = hashlib.sha1()
    for blob in (kernel, ramdisk, b""):  # kernel, ramdisk, second
        sha.update(blob)
        sha.update(struct.pack("<I", len(blob)))
    img_id = sha.digest().ljust(32, b"\0")

    hdr = struct.pack(
        HDR_FMT, MAGIC,
        len(kernel), a.base + a.kernel_offset,
        len(ramdisk), a.base + a.ramdisk_offset,
        0, a.base + a.second_offset,
        a.base + a.tags_offset, page,
        0,  # header_version / Samsung dt_size
        0,  # os_version
        a.board.encode()[:15], a.cmdline.encode()[:511], img_id, b"")

    out = pad(hdr, page) + pad(kernel, page) + (pad(ramdisk, page) if ramdisk else b"")
    if a.seandroid:
        out += SEANDROID
    open(a.output, "wb").write(out)
    print(f"wrote {a.output}: {len(out)} bytes, kernel {len(kernel)}, ramdisk {len(ramdisk)}")


def info(path):
    data = open(path, "rb").read()
    f = struct.unpack_from(HDR_FMT, data)
    if f[0] != MAGIC:
        sys.exit("not an Android boot image")
    names = ["kernel_size", "kernel_addr", "ramdisk_size", "ramdisk_addr",
             "second_size", "second_addr", "tags_addr", "page_size",
             "header_version/dt_size", "os_version"]
    for n, v in zip(names, f[1:11]):
        print(f"{n:24s} 0x{v:08x} ({v})")
    board = f[11].split(b"\0")[0].decode(errors="replace")
    cmdline = f[12].split(b"\0")[0].decode(errors="replace")
    print(f"{'board':24s} {board!r}")
    print(f"{'cmdline':24s} {cmdline!r}")
    page = f[8]
    koff = page
    roff = koff + ((f[1] + page - 1) // page) * page
    kernel = data[koff:koff + f[1]]
    ramdisk = data[roff:roff + f[3]]
    sha = hashlib.sha1()
    for blob in (kernel, ramdisk, b""):
        sha.update(blob)
        sha.update(struct.pack("<I", len(blob)))
    ok = sha.digest() == f[13][:20]
    print(f"{'id (sha1) matches':24s} {ok}")
    print(f"{'SEANDROIDENFORCE tail':24s} {data.endswith(SEANDROID)}")
    return kernel, ramdisk, ok


def main():
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="cmd", required=True)
    pk = sub.add_parser("pack")
    pk.add_argument("--kernel", required=True)
    pk.add_argument("--ramdisk")
    pk.add_argument("-o", "--output", required=True)
    pk.add_argument("--base", type=lambda x: int(x, 0), default=0x40000000)
    pk.add_argument("--kernel_offset", type=lambda x: int(x, 0), default=0x00000000)
    pk.add_argument("--ramdisk_offset", type=lambda x: int(x, 0), default=0x01000000)
    pk.add_argument("--second_offset", type=lambda x: int(x, 0), default=0x00f00000)
    pk.add_argument("--tags_offset", type=lambda x: int(x, 0), default=0x00000100)
    pk.add_argument("--pagesize", type=int, default=2048)
    pk.add_argument("--board", default="")
    pk.add_argument("--cmdline", default="")
    pk.add_argument("--no-seandroid", dest="seandroid", action="store_false")
    ip = sub.add_parser("info")
    ip.add_argument("image")
    a = p.parse_args()
    if a.cmd == "pack":
        pack(a)
    else:
        info(a.image)


if __name__ == "__main__":
    main()
