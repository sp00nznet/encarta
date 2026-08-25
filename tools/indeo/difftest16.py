"""Differential-test the lifted 16-bit code against real x86.

Same idea as difftest.py, for the other lifter. Segment 3's semantics came out
clean, which moves the question to the 16-bit half - and that is where the
decode is actually driven from: DriverProc dispatches ICM_DECOMPRESS, 7:0714
validates the frame, and 5:06C8 sets up the planes and calls the 32-bit
workers. A wrong translation there hands the codec wrong parameters, which
looks exactly like a codec that runs and produces the wrong picture.

lift16's output does not carry the instruction address in its comment, so
instructions cannot be matched to bytes the way difftest.py does it. Instead
drive the lifter directly: decode the segment, lift each instruction on its
own, and test that.

    python difftest16.py <IR32.DLL> --seg 7
"""
import argparse
import os
import re
import struct
import sys

MEM_SIZE = 0x20000
R16 = ["ax", "cx", "dx", "bx", "sp", "bp", "si", "di"]


def rnd(state):
    state[0] = (state[0] * 1103515245 + 12345) & 0xFFFFFFFF
    return state[0]


def testable(stmt, text):
    if any(k in stmt for k in ("goto", "return", "recomp_dispatch", "ir32_s",
                               "ir32_enter", "abort", "UNHANDLED", "int_handler",
                               "push16", "pop16", "push32", "pop32", "res_",
                               "catz_div0", "dos_int")):
        return False
    if re.search(r"cpu->(ds|es|fs|gs|ss|cs)\s*=", stmt):
        return False
    # A cs: override cannot be compared: the reference needs CS to carry the
    # high bits of the code address (16-bit IP is only 16 bits wide), while
    # this runtime resolves every selector through SEG_OFF. The two therefore
    # read different addresses by construction, which is a property of the
    # harness and not a finding.
    if "cs:" in text:
        return False
    if text.startswith(("rep", "movs", "stos", "lods", "scas", "cmps", "int",
                        "call", "j", "loop", "ret", "push", "pop", "lds",
                        "les", "nop", "in ", "out", "hlt", "iret", "cli",
                        "sti", "enter", "leave")):
        return False
    return True


def collect(a):
    sys.path.insert(0, os.path.join(a.pcrecomp, "tools", "disasm"))
    sys.path.insert(0, os.path.join(a.pcrecomp, "tools", "ne"))
    sys.path.insert(0, os.path.join(a.pcrecomp, "tools", "lift"))
    import ne_parse
    import decode16
    import lift16

    ne = ne_parse.parse_ne(a.ne_file)
    seg = [s for s in ne.code_segments if s.index == a.seg][0]
    code = bytes(seg.data)

    insns, off = [], 0
    while off < len(code):
        d = decode16.Decoder(code, 0)
        d.pos = off
        try:
            i = d.decode_one()
        except Exception:
            off += 1
            continue
        if i is None or i.length <= 0 or (i.mnemonic or "").lower() == "db":
            off += 1
            continue
        insns.append(i)
        off += i.length

    lifter = lift16.Lifter()
    seen, tests = set(), []
    pat = re.compile(r"^\s*(.+?)\s*/\* (.+?) \*/\s*$")
    for i in insns:
        raw = code[i.address:i.address + i.length]
        if raw in seen:
            continue
        seen.add(raw)
        try:
            c = lifter.lift_function("probe", [i], i.address)
        except Exception:
            continue
        if isinstance(c, list):
            c = "\n".join(c)
        for line in c.split("\n"):
            m = pat.match(line)
            if not m:
                continue
            stmt, text = m.group(1), m.group(2)
            if not stmt.endswith(";") and not stmt.endswith("}"):
                continue
            if testable(stmt, text):
                tests.append((raw, stmt, text))
            break
    return tests


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ne_file")
    ap.add_argument("--seg", type=int, required=True,
                    help="16-bit segments only; 2 and 3 are 32-bit in this DLL "
                         "and belong to difftest.py")
    ap.add_argument("--pcrecomp", default=r"G:\recomp\pc\tools")
    ap.add_argument("--out", required=True)
    ap.add_argument("--vectors", type=int, default=2)
    a = ap.parse_args()

    tests = collect(a)
    print("segment %d: %d distinct testable instructions" % (a.seg, len(tests)))

    with open(os.path.join(a.out, "dt16_gen.c"), "w") as f:
        f.write(HEAD)
        for n, (raw, stmt, text) in enumerate(tests):
            f.write("static void t%d(CPU *cpu) { %s }  /* %s */\n"
                    % (n, stmt, text.replace("*/", "* /")))
        f.write("\nstatic void (*const TESTS[])(CPU *) = {\n")
        f.write("".join("    t%d,\n" % n for n in range(len(tests))))
        f.write("};\n")
        f.write(TAIL)

    # reference side
    import capstone
    from unicorn import Uc, UC_ARCH_X86, UC_MODE_16, UC_PROT_ALL
    from unicorn import x86_const as X
    REG = {n: getattr(X, "UC_X86_REG_" + n.upper()) for n in R16}
    pattern = bytes(((i * 7 + 13) & 0xFF) for i in range(MEM_SIZE))
    CODE_AT = 0x30000

    rows = []
    for t, (raw, stmt, text) in enumerate(tests):
        for v in range(a.vectors):
            st = [(t * 977 + v * 7919 + 1) & 0xFFFFFFFF]
            vals = {n: rnd(st) & 0xFFFF for n in R16}
            fl = rnd(st)
            uc = Uc(UC_ARCH_X86, UC_MODE_16)
            uc.mem_map(0, MEM_SIZE, UC_PROT_ALL)
            uc.mem_map(CODE_AT, 0x1000, UC_PROT_ALL)
            uc.mem_write(0, pattern)
            uc.mem_write(CODE_AT, raw + b"\xf4")
            for n in R16:
                uc.reg_write(REG[n], vals[n])
            # Data segments at zero so the offset IS the linear address, which
            # is what SEG_OFF gives on the other side. CS cannot be: in 16-bit
            # mode the fetch address is CS*16 + IP with IP only 16 bits wide,
            # so a code address of 0x30000 with CS=0 truncates to 0 and the
            # emulator executes the data pattern instead of the instruction.
            for s in ("ds", "es", "ss"):
                uc.reg_write(getattr(X, "UC_X86_REG_" + s.upper()), 0)
            uc.reg_write(X.UC_X86_REG_CS, CODE_AT >> 4)
            eflags = 0x2 | (fl & 1) | ((fl >> 1 & 1) << 2) | \
                     ((fl >> 2 & 1) << 6) | ((fl >> 3 & 1) << 7) | \
                     ((fl >> 4 & 1) << 11)
            uc.reg_write(X.UC_X86_REG_EFLAGS, eflags)
            try:
                uc.emu_start(CODE_AT, CODE_AT + len(raw), count=1)
            except Exception as e:
                rows.append("%d %d ERROR" % (t, v))
                continue
            got = [uc.reg_read(REG[n]) for n in R16]
            e = uc.reg_read(X.UC_X86_REG_EFLAGS)
            fo = ((e >> 0) & 1, (e >> 6) & 1, (e >> 7) & 1,
                  (e >> 11) & 1, (e >> 2) & 1)
            mem = uc.mem_read(0, MEM_SIZE)
            crc = 0
            for o in range(0, MEM_SIZE, 4):
                crc = ((crc * 16777619) ^ struct.unpack_from("<I", mem, o)[0]) & 0xFFFFFFFF
            rows.append("%d %d %s %s %08X"
                        % (t, v, " ".join("%04X" % x for x in got),
                           "".join(str(x) for x in fo), crc))
    with open(os.path.join(a.out, "dt16_ref.txt"), "w") as f:
        f.write("\n".join(rows) + "\n")
    with open(os.path.join(a.out, "dt16_text.txt"), "w") as f:
        for raw, stmt, text in tests:
            f.write("%s\t%s\n" % (text, stmt))
    print("wrote dt16_gen.c and dt16_ref.txt (%d rows)" % len(rows))


HEAD = r'''/* AUTO-GENERATED by difftest16.py */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
/* Not MEM_SIZE: recomp16/cpu.h defines that as 1 MB + 64 K and its
 * definition wins, which made the fill loop run a megabyte into a
 * 128 K array and take the harness down before the first test. */
#define DT_MEM 0x20000
static unsigned char g_mem[DT_MEM];
uint32_t g_segoff[65536];
/* every selector at zero, so the offset IS the address - which is what the
 * Unicorn side computes with all segment registers set to zero */
#define SEG_OFF(seg, off) ((uint32_t)(uint16_t)(off))
#include "recomp16/cpu.h"
'''

TAIL = r'''
static uint32_t rnd(uint32_t *s){ *s = (*s*1103515245u+12345u); return *s; }

int main(int argc, char **argv)
{
    unsigned nv = argc > 1 ? (unsigned)atoi(argv[1]) : 2;
    unsigned n = (unsigned)(sizeof TESTS / sizeof TESTS[0]);
    /* Unbuffered: if one test faults, the last line printed names it. With
     * buffering the whole run's output is lost and the crash says nothing. */
    setvbuf(stdout, NULL, _IONBF, 0);
    for (unsigned t = 0; t < n; t++) {
        for (unsigned v = 0; v < nv; v++) {
            uint32_t s = t*977u + v*7919u + 1u;
            CPU cpu; memset(&cpu, 0, sizeof cpu);
            cpu.mem = g_mem;
            cpu.ax = (uint16_t)rnd(&s); cpu.cx = (uint16_t)rnd(&s);
            cpu.dx = (uint16_t)rnd(&s); cpu.bx = (uint16_t)rnd(&s);
            cpu.sp = (uint16_t)rnd(&s); cpu.bp = (uint16_t)rnd(&s);
            cpu.si = (uint16_t)rnd(&s); cpu.di = (uint16_t)rnd(&s);
            uint32_t fl = rnd(&s);
            cpu.flags = 0x2
                      | ((fl    & 1))
                      | (((fl>>1)&1) << 2)
                      | (((fl>>2)&1) << 6)
                      | (((fl>>3)&1) << 7)
                      | (((fl>>4)&1) << 11);
            for (unsigned i = 0; i < DT_MEM; i++)
                g_mem[i] = (unsigned char)((i*7+13) & 0xFF);
            TESTS[t](&cpu);
            unsigned long crc = 0;
            for (unsigned i = 0; i < DT_MEM; i += 4) {
                unsigned long w = *(uint32_t *)(g_mem + i);
                crc = crc * 16777619ul ^ w;
            }
            printf("%u %u %04X %04X %04X %04X %04X %04X %04X %04X %u%u%u%u%u %08lX\n",
                   t, v, cpu.ax, cpu.cx, cpu.dx, cpu.bx, cpu.sp, cpu.bp,
                   cpu.si, cpu.di,
                   (cpu.flags>>0)&1, (cpu.flags>>6)&1, (cpu.flags>>7)&1,
                   (cpu.flags>>11)&1, (cpu.flags>>2)&1,
                   crc);
        }
    }
    return 0;
}
'''

if __name__ == "__main__":
    main()
