#!/usr/bin/env python3
"""Run the mainline boot image (U-Boot + FIT) on an emulated Cortex-A53.

Emulates what S-Boot does: put the boot image "kernel" (u-boot.bin) at a load
address, the "ramdisk" (FIT) somewhere else, and pass x0 = a device tree whose
/chosen has linux,initrd-start/end (U-Boot reads these:
arch/arm/lib/save_prev_bl_data.c). All SoC registers read as 0; writes to the
UART2 transmit register (0x13820020) are captured as U-Boot's console.

Stops when the CPU reaches the Linux kernel entry, then decodes the device
tree U-Boot hands to Linux (x0) and prints memory, reserved-memory and /chosen.

Proves: U-Boot runs from an unlinked address, finds and verifies the FIT, runs
the boot script, places kernel/initrd/fdt, fixes up the DT, jumps to Linux.
Does not prove: real S-Boot addresses, real peripheral behaviour, Linux boot.
--follow-linux keeps running the kernel, but Unicorn 2.1.4 raises an
unhandled prefetch-abort right after Linux enables its MMU (__enable_mmu),
so Linux itself can only be tested on the phone.

Usage: emulate_uboot.py BOOT_IMG [--uboot-at ADDR] [--fit-at ADDR] [--dtc PATH]
Env:   LIBUNICORN_PATH, PYTHONPATH as in run-emulator.sh
"""
import argparse
import struct
import subprocess
import sys
import tempfile

from unicorn import Uc, UcError, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_BLOCK, \
    UC_HOOK_MEM_UNMAPPED, UC_PROT_ALL
from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_X0, UC_CPU_ARM64_A53

RAM_BASE, RAM_END = 0x40000000, 0x100000000
MMIO_BASE, MMIO_SIZE = 0x10000000, 0x6000000
UART_TX = 0x13820020
KERNEL_ENTRY = 0x48000000

console = bytearray()
mmio_seen = {}


def mmio_read(uc, off, size, _):
    mmio_seen[MMIO_BASE + off] = mmio_seen.get(MMIO_BASE + off, 0) + 1
    return 0


def mmio_write(uc, off, size, value, _):
    addr = MMIO_BASE + off
    if addr == UART_TX:
        console.append(value & 0xff)
    else:
        mmio_seen[addr] = mmio_seen.get(addr, 0) + 1


def make_prev_fdt(dtc, initrd_start, initrd_end):
    src = f"""/dts-v1/;
/ {{ #address-cells = <2>; #size-cells = <2>;
  chosen {{ linux,initrd-start = <0x0 {initrd_start:#x}>;
            linux,initrd-end = <0x0 {initrd_end:#x}>; }};
  memory@40000000 {{ device_type = "memory"; reg = <0x0 0x40000000 0x0 0x3e400000>; }};
}};"""
    with tempfile.NamedTemporaryFile("w", suffix=".dts") as f:
        f.write(src)
        f.flush()
        return subprocess.run([dtc, "-I", "dts", "-O", "dtb", f.name],
                              check=True, capture_output=True).stdout


def decode_fdt(blob, dtc):
    with tempfile.NamedTemporaryFile(suffix=".dtb") as f:
        f.write(blob)
        f.flush()
        return subprocess.run([dtc, "-I", "dtb", "-O", "dts", f.name],
                              capture_output=True, text=True).stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("boot_img")
    ap.add_argument("--uboot-at", type=lambda x: int(x, 0), default=0x40080000)
    ap.add_argument("--fit-at", type=lambda x: int(x, 0), default=0x41000000)
    ap.add_argument("--dtc", default="dtc")
    ap.add_argument("--timeout", type=int, default=600)
    ap.add_argument("--follow-linux", type=int, default=0, metavar="SECONDS",
                    help="keep running Linux this long, then dump its log and screen")
    ap.add_argument("--log-buf-off", type=lambda x: int(x, 0), default=0,
                    help="__log_buf - _text from vmlinux (for --follow-linux)")
    ap.add_argument("--png", default="")
    a = ap.parse_args()

    img = open(a.boot_img, "rb").read()
    assert img[:8] == b"ANDROID!"
    ksz, _, rsz = struct.unpack_from("<III", img, 8)
    page = struct.unpack_from("<I", img, 36)[0]
    uboot = img[page:page + ksz]
    roff = page + ((ksz + page - 1) // page) * page
    fit = img[roff:roff + rsz]

    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM, cpu=UC_CPU_ARM64_A53)
    uc.mem_map(RAM_BASE, RAM_END - RAM_BASE, UC_PROT_ALL)
    uc.mmio_map(MMIO_BASE, MMIO_SIZE, mmio_read, None, mmio_write, None)

    def unmapped(uc, access, addr, size, value, _):
        print(f"!! unmapped access {access} at {addr:#x} pc={uc.reg_read(UC_ARM64_REG_PC):#x}")
        return False
    uc.hook_add(UC_HOOK_MEM_UNMAPPED, unmapped)

    prev_fdt_at = 0x44000000
    uc.mem_write(a.uboot_at, uboot)
    uc.mem_write(a.fit_at, fit)
    uc.mem_write(prev_fdt_at, make_prev_fdt(a.dtc, a.fit_at, a.fit_at + len(fit)))

    state = {"entered": False}

    def on_block(uc, addr, size, _):
        if addr == KERNEL_ENTRY and not state["entered"]:
            state["entered"] = True
            uc.emu_stop()
    uc.hook_add(UC_HOOK_BLOCK, on_block)

    uc.reg_write(UC_ARM64_REG_X0, prev_fdt_at)
    try:
        uc.emu_start(a.uboot_at, 0, timeout=a.timeout * 1_000_000)
    except UcError as e:
        print(f"!! emulation error: {e} at pc={uc.reg_read(UC_ARM64_REG_PC):#x}")

    print("----- U-Boot console (UART2) -----")
    print(console.decode(errors="replace").replace("\r", ""))
    print("----------------------------------")
    pc = uc.reg_read(UC_ARM64_REG_PC)
    print(f"u-boot loaded at {a.uboot_at:#x}, FIT at {a.fit_at:#x}; stopped pc={pc:#x}")
    if not state["entered"]:
        print("RESULT: did NOT reach the Linux entry point")
        sys.exit(1)

    x0 = uc.reg_read(UC_ARM64_REG_X0)
    hdr = bytes(uc.mem_read(x0, 8))
    magic, total = struct.unpack(">II", hdr)
    print(f"RESULT: reached Linux entry {KERNEL_ENTRY:#x}, x0 (fdt) = {x0:#x}, "
          f"fdt magic {magic:#x}, size {total}")
    image_hdr = bytes(uc.mem_read(KERNEL_ENTRY, 64))
    print(f"Image magic at entry+0x38: {image_hdr[0x38:0x3c]!r}")
    dts = decode_fdt(bytes(uc.mem_read(x0, total)), a.dtc)
    keep = ("memory", "reg =", "no-map", "linux,initrd", "bootargs",
            "reserved-memory", "chosen", "framebuffer", "@")
    for line in dts.splitlines():
        if any(k in line for k in keep) and "cpu@" not in line and "core" not in line:
            print("  " + line.strip())

    if not a.follow_linux:
        return
    print(f"----- following Linux for {a.follow_linux} s (no interrupts emulated) -----")
    try:
        uc.emu_start(KERNEL_ENTRY, 0, timeout=a.follow_linux * 1_000_000)
    except UcError as e:
        print(f"!! linux stopped: {e} at pc={uc.reg_read(UC_ARM64_REG_PC):#x}")
    if a.log_buf_off:
        buf = bytes(uc.mem_read(KERNEL_ENTRY + a.log_buf_off, 1 << 17))
        import re
        msgs = [m.decode() for m in re.findall(rb"[\x20-\x7e]{6,}", buf)]
        print(f"----- Linux printk text ({len(msgs)} records) -----")
        for m in msgs:
            print("  " + m)
    if a.png:
        W, H = 1080, 1920
        fb = bytes(uc.mem_read(0x67000000, W * H * 4))
        rgba = bytearray(len(fb))
        rgba[0::4], rgba[1::4], rgba[2::4] = fb[2::4], fb[1::4], fb[0::4]
        rgba[3::4] = b"\xff" * (len(fb) // 4)
        import zlib
        raw = b"".join(b"\0" + rgba[y * W * 4:(y + 1) * W * 4] for y in range(H))
        def chunk(t, d):
            return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
        open(a.png, "wb").write(b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))
        print(f"screen -> {a.png}")


if __name__ == "__main__":
    main()
