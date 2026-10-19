#!/usr/bin/env python3
"""Tiny aarch64 Linux user-mode runner on Unicorn, for smoke-testing static
binaries built for the phone (no qemu-user in this workspace).

Supports only what small static musl programs need at start-up and for
printing: execve-style stack, brk, anonymous mmap, write/writev, exit, and a
handful of harmless syscalls that return 0 or -ENOSYS. Anything else is
reported and returns -ENOSYS.

Usage: uemu.py BINARY [ARGS...]
Env:   LIBUNICORN_PATH, PYTHONPATH (see tools/run-emulator.sh)
"""
import struct
import sys

from unicorn import Uc, UcError, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_INTR, \
    UC_PROT_ALL
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, \
    UC_ARM64_REG_X2, UC_ARM64_REG_X8, UC_ARM64_REG_SP, UC_ARM64_REG_PC, \
    UC_CPU_ARM64_A53, UC_ARM64_REG_TPIDR_EL0

PAGE = 0x1000
STACK_TOP = 0x7ff0000000
STACK_SIZE = 0x100000
MMAP_BASE = 0x7f00000000
ENOSYS, ENOTTY = 38, 25


def align(x, a=PAGE):
    return (x + a - 1) & ~(a - 1)


def main():
    path, argv = sys.argv[1], sys.argv[1:]
    data = open(path, "rb").read()
    assert data[:4] == b"\x7fELF" and data[4] == 2, "not ELF64"
    e_entry, e_phoff = struct.unpack_from("<QQ", data, 24)
    e_phentsize, e_phnum = struct.unpack_from("<HH", data, 54)

    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM, cpu=UC_CPU_ARM64_A53)
    brk = 0
    phdr_addr = 0
    for i in range(e_phnum):
        p_type, _, p_off, p_vaddr, _, p_filesz, p_memsz, _ = struct.unpack_from(
            "<IIQQQQQQ", data, e_phoff + i * e_phentsize)
        if p_type == 6:
            phdr_addr = p_vaddr
        if p_type != 1:
            continue
        lo, hi = p_vaddr & ~(PAGE - 1), align(p_vaddr + p_memsz)
        uc.mem_map(lo, hi - lo, UC_PROT_ALL)
        uc.mem_write(p_vaddr, data[p_off:p_off + p_filesz])
        if p_off == 0 and not phdr_addr:
            phdr_addr = p_vaddr + e_phoff
        brk = max(brk, hi)
    heap = {"brk": brk, "mapped": brk, "mmap": MMAP_BASE}

    uc.mem_map(STACK_TOP - STACK_SIZE, STACK_SIZE, UC_PROT_ALL)
    sp = STACK_TOP - 0x1000
    strs = []
    for a in argv + ["PATH=/bin"]:
        b = a.encode() + b"\0"
        sp -= len(b)
        uc.mem_write(sp, b)
        strs.append(sp)
    sp -= 16
    uc.mem_write(sp, b"A53RANDOMBYTES!!")
    rnd = sp
    sp &= ~15
    auxv = [(3, phdr_addr), (4, e_phentsize), (5, e_phnum), (6, PAGE),
            (9, e_entry), (25, rnd), (0, 0)]
    words = [len(argv)] + strs[:len(argv)] + [0] + strs[len(argv):] + [0]
    for k, v in auxv:
        words += [k, v]
    if len(words) % 2:
        words.append(0)
    sp -= 8 * len(words)
    uc.mem_write(sp, struct.pack("<%dQ" % len(words), *words))
    uc.reg_write(UC_ARM64_REG_SP, sp)

    out = sys.stdout.buffer

    def ret(v):
        uc.reg_write(UC_ARM64_REG_X0, v & 0xffffffffffffffff)

    def on_intr(uc, intno, _):
        nr = uc.reg_read(UC_ARM64_REG_X8)
        a0, a1, a2 = (uc.reg_read(r) for r in
                      (UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2))
        if nr == 64:                       # write
            out.write(bytes(uc.mem_read(a1, a2)))
            ret(a2)
        elif nr == 66:                     # writev
            total = 0
            for i in range(a2):
                base, ln = struct.unpack("<QQ", uc.mem_read(a1 + 16 * i, 16))
                if ln:
                    out.write(bytes(uc.mem_read(base, ln)))
                total += ln
            ret(total)
        elif nr in (93, 94):               # exit, exit_group
            out.flush()
            print(f"\n[uemu] exit({a0})", file=sys.stderr)
            uc.emu_stop()
            heap["exit"] = a0
        elif nr == 214:                    # brk
            if a0 > heap["brk"]:
                need = align(a0)
                if need > heap["mapped"]:
                    uc.mem_map(heap["mapped"], need - heap["mapped"], UC_PROT_ALL)
                    heap["mapped"] = need
                heap["brk"] = a0
            ret(heap["brk"])
        elif nr == 222:                    # mmap (anonymous only)
            ln = align(a1)
            addr = heap["mmap"]
            uc.mem_map(addr, ln, UC_PROT_ALL)
            heap["mmap"] += ln + PAGE
            ret(addr)
        elif nr == 29:                     # ioctl
            ret(-ENOTTY)
        elif nr == 160:                    # uname
            buf = b""
            for f in (b"Linux", b"a5y17lte", b"7.2.0", b"#1", b"aarch64", b""):
                buf += f.ljust(65, b"\0")
            uc.mem_write(a0, buf)
            ret(0)
        elif nr in (96, 99, 134, 135, 174, 175, 176, 177, 178, 226, 215, 261):
            ret(0 if nr != 178 else 1)     # tid/robust/sigs/ids/mprotect...
        else:
            print(f"[uemu] syscall {nr} -> ENOSYS", file=sys.stderr)
            ret(-ENOSYS)

    uc.hook_add(UC_HOOK_INTR, on_intr)
    try:
        uc.emu_start(e_entry, 0, timeout=30_000_000)
    except UcError as err:
        print(f"[uemu] error {err} pc={uc.reg_read(UC_ARM64_REG_PC):#x}",
              file=sys.stderr)
        sys.exit(2)
    sys.exit(heap.get("exit", 3))


if __name__ == "__main__":
    main()
