#!/usr/bin/env python3
"""Run the step-1 boot image in an emulated Cortex-A53 and render the screen.

What this proves (and what it does not):
  + the uniLoader payload inside the real boot.img self-relocates from the
    S-Boot load address to TEXT_BASE and reaches the diagnostics code
  + every MMIO access it makes is to an expected Exynos 7880 block
  + text and colour bars land in the framebuffer at 0x67000000
  - real S-Boot entry state, real DECON scan-out and real register values
    can only be confirmed on the phone

Usage: emulate_step1.py BOOT_IMG OUT_PNG [--load 0x40000000]
Env:   LIBUNICORN_PATH=<dir with libunicorn.so>
"""
import argparse
import struct
import sys
import zlib

from unicorn import Uc, UcError, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_BLOCK, \
    UC_HOOK_MEM_UNMAPPED, UC_PROT_ALL
from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_X0, \
    UC_CPU_ARM64_A53

FB_BASE, FB_W, FB_H = 0x67000000, 1080, 1920
RAM_BASE, RAM_SIZE = 0x40000000, 0x3E400000          # DRAM bank 0
DECON_BASE, IDMA_BASE = 0x14830000, 0x14821000
PMU_BASE = 0x106B0000

mmio_log = []
# Plausible boot-time state: S-Boot scans out from IDMA G0 at 0x67000000.
idma_regs = {IDMA_BASE + 0x74: FB_BASE}
decon_regs = {}


def decon_read(uc, off, size, data):
    v = decon_regs.get(off, 0)
    mmio_log.append(("R", DECON_BASE + off, v))
    return v


def decon_write(uc, off, size, value, data):
    decon_regs[off] = value
    mmio_log.append(("W", DECON_BASE + off, value))


def idma_read(uc, off, size, data):
    v = idma_regs.get(IDMA_BASE + off, 0)
    mmio_log.append(("R", IDMA_BASE + off, v))
    return v


def idma_write(uc, off, size, value, data):
    mmio_log.append(("W", IDMA_BASE + off, value))


pmu = {"reset": False}


def pmu_read(uc, off, size, data):
    mmio_log.append(("R", PMU_BASE + off, 0))
    return 0


def pmu_write(uc, off, size, value, data):
    mmio_log.append(("W", PMU_BASE + off, value))
    if off == 0x400 and value == 1:
        pmu["reset"] = True
        uc.emu_stop()


def unmapped(uc, access, addr, size, value, data):
    print(f"!! unmapped access type={access} addr=0x{addr:x} size={size} "
          f"pc=0x{uc.reg_read(UC_ARM64_REG_PC):x}")
    return False


def write_png(path, rgba, w, h):
    raw = b"".join(b"\0" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))

    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("boot_img")
    ap.add_argument("out_png")
    ap.add_argument("--load", type=lambda x: int(x, 0), default=0x40000000)
    ap.add_argument("--timeout", type=int, default=120, help="seconds")
    a = ap.parse_args()

    img = open(a.boot_img, "rb").read()
    assert img[:8] == b"ANDROID!", "not a boot image"
    ksize, page = struct.unpack_from("<I", img, 8)[0], struct.unpack_from("<I", img, 36)[0]
    kernel = img[page:page + ksize]

    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM, cpu=UC_CPU_ARM64_A53)
    uc.mem_map(RAM_BASE, RAM_SIZE, UC_PROT_ALL)
    uc.mmio_map(DECON_BASE, 0x10000, decon_read, None, decon_write, None)
    uc.mmio_map(IDMA_BASE, 0x6000, idma_read, None, idma_write, None)
    uc.mmio_map(PMU_BASE, 0x1000, pmu_read, None, pmu_write, None)
    uc.hook_add(UC_HOOK_MEM_UNMAPPED, unmapped)

    uc.mem_write(a.load, kernel)

    state = {"wfe_hits": 0, "insns": 0}

    def on_block(uc, addr, size, _):
        state["insns"] += 1  # counts basic blocks
        code = bytes(uc.mem_read(addr, size))
        if not pmu["reset"] and any(
                struct.unpack_from("<I", code, i)[0] == 0xd503205f
                for i in range(0, size - 3, 4)):  # block contains wfe
            state["wfe_hits"] += 1
            if state["wfe_hits"] > 3:
                uc.emu_stop()
    uc.hook_add(UC_HOOK_BLOCK, on_block)

    uc.reg_write(UC_ARM64_REG_X0, 0)
    try:
        uc.emu_start(a.load, 0, timeout=a.timeout * 1_000_000)
    except UcError as e:
        print(f"!! emulation error: {e} at pc=0x{uc.reg_read(UC_ARM64_REG_PC):x}")

    pc = uc.reg_read(UC_ARM64_REG_PC)
    print(f"load=0x{a.load:x} stopped pc=0x{pc:x} blocks={state['insns']} "
          f"halted_in_wfe={state['wfe_hits'] > 3} pmu_reset={pmu['reset']}")
    trig = [v for k, ad, v in mmio_log if k == "W" and ad == DECON_BASE + 0x70]
    print(f"DECON trigger writes: {len(trig)} "
          f"(fire=0x1100: {trig.count(0x1100)}, idle=0x1000: {trig.count(0x1000)})")
    print("MMIO accesses (unique):")
    seen = set()
    for kind, addr, val in mmio_log:
        if (kind, addr) not in seen:
            seen.add((kind, addr))
            print(f"  {kind} 0x{addr:08x} = 0x{val:08x}")

    fb = bytes(uc.mem_read(FB_BASE, FB_W * FB_H * 4))
    # ARGB8888 little-endian in memory is B,G,R,A -> convert to RGBA
    rgba = bytearray(len(fb))
    rgba[0::4], rgba[1::4], rgba[2::4] = fb[2::4], fb[1::4], fb[0::4]
    rgba[3::4] = b"\xff" * (len(fb) // 4)
    write_png(a.out_png, bytes(rgba), FB_W, FB_H)
    print(f"screen -> {a.out_png}")
    sys.exit(0 if (pmu["reset"] or state["wfe_hits"] > 3) else 1)


if __name__ == "__main__":
    main()
