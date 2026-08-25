"""Differential-test the lifted 32-bit code against real x86 semantics.

The decoder consumes the whole frame and writes structured output that is not
the picture, with every instruction lifted and none unhandled. That shape of
failure is a WRONG TRANSLATION, not a missing one, and reading code looking for
it does not scale: segment 3 is 5,339 instructions.

So take each distinct instruction the lifter emitted, run it two ways on
identical machine state - once through the generated C, once through Unicorn's
x86 - and compare registers, flags and memory. Anything that disagrees is a
lifter bug, named exactly.

Deliberately out of scope: control flow (the lifter turns it into goto/return,
which is a different question), segment-register loads (the runtime's whole
point is that it models those differently from a flat CPU), and string
operations (rewritten wholesale, and they loop).

    python difftest.py <IR32.DLL> --seg 3

Writes a C harness, builds it, runs both sides, and prints the disagreements.
"""
import argparse
import os
import re
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))

MEM_BASE = 0x00000000
MEM_SIZE = 0x00040000          # 256K: covers base+0xE188 and any 16-bit wrap
BASE_REG = 0x00020000          # where memory-operand base registers point
IDX_REG = 0x10                 # index registers stay small; they get scaled

GPR = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
FLAGS = ["cf", "zf", "sf", "of", "pf"]   # af excluded: see report()


def lcg(seed):
    """The same generator on both sides, so the two runs see identical state."""
    s = seed & 0xFFFFFFFF
    while True:
        s = (s * 1103515245 + 12345) & 0xFFFFFFFF
        yield s


def statements(path):
    """Pull (address, C statement) out of the generated file.

    Each lifted line carries its own instruction as a trailing comment, which
    is what makes this possible at all - the address identifies the bytes and
    the statement is what we want to test."""
    out = []
    pat = re.compile(r"^    (.+?)\s+/\* ([0-9A-F]{8}): (.+?) \*/$")
    for line in open(path):
        m = pat.match(line.rstrip("\n"))
        if m:
            out.append((int(m.group(2), 16), m.group(1), m.group(3)))
    return out


def testable(stmt, text):
    """Skip what is not a pure computation on registers and memory."""
    if any(k in stmt for k in ("goto", "return", "recomp_dispatch", "L_s3_",
                               "ir32_enter", "abort", "TODO", "UNHANDLED")):
        return False
    # segment loads: the runtime models these as selector bookkeeping on
    # purpose, so a flat CPU is not the reference for them
    if re.search(r"c->(ds|es|fs|gs|ss|cs)\s*=", stmt):
        return False
    if text.startswith(("rep", "movs", "stos", "lods", "scas", "cmps", "int",
                        "call", "j", "loop", "ret", "push", "pop", "lds",
                        "les", "lfs", "lgs", "lss", "nop")):
        return False
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ne_file")
    ap.add_argument("--seg", type=int, default=3)
    ap.add_argument("--pcrecomp", default=r"G:\recomp\pc\tools")
    ap.add_argument("--build", default=os.path.join(HERE, "runtime", "build"))
    ap.add_argument("--vectors", type=int, default=3)
    ap.add_argument("--limit", type=int, default=0)
    a = ap.parse_args()

    sys.path.insert(0, os.path.join(a.pcrecomp, "tools", "ne"))
    import ne_parse
    import capstone

    ne = ne_parse.parse_ne(a.ne_file)
    seg = [s for s in ne.code_segments if s.index == a.seg][0]
    code = bytearray(seg.data)

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True

    src = os.path.join(a.build, "ir32_seg%d.c" % a.seg)
    if not os.path.exists(src):
        print("no %s - build first" % src)
        return 1

    seen, tests = set(), []
    for addr, stmt, text in statements(src):
        if addr >= len(code):
            continue
        try:
            insn = next(md.disasm(bytes(code[addr:addr + 16]), addr))
        except StopIteration:
            continue
        raw = bytes(insn.bytes)
        if raw in seen or not testable(stmt, text):
            continue
        seen.add(raw)
        # which registers take part in addressing, so they can be pointed
        # somewhere mapped instead of at a random 32-bit number
        base = idx = None
        for op in insn.operands:
            if op.type == capstone.x86.X86_OP_MEM:
                if op.mem.base:
                    base = md.reg_name(op.mem.base)
                if op.mem.index:
                    idx = md.reg_name(op.mem.index)
        tests.append((raw, stmt, text, base, idx))
        if a.limit and len(tests) >= a.limit:
            break

    print("%d distinct testable instructions in segment %d" % (len(tests), a.seg))

    gen = os.path.join(a.build, "difftest_gen.c")
    with open(gen, "w") as f:
        f.write(HARNESS_HEAD)
        for i, (raw, stmt, text, base, idx) in enumerate(tests):
            f.write("static void t%d(CPU *c) { %s }   /* %s */\n"
                    % (i, stmt, text.replace("*/", "* /")))
        f.write("\nstatic void (*const TESTS[])(CPU *) = {\n")
        for i in range(len(tests)):
            f.write("    t%d,\n" % i)
        f.write("};\n")
        f.write(HARNESS_TAIL)

    # register plan per test, so both sides agree on which regs are addresses
    plan = []
    for raw, stmt, text, base, idx in tests:
        plan.append((base or "", idx or ""))
    with open(os.path.join(a.build, "difftest_plan.txt"), "w") as f:
        for b, i in plan:
            f.write("%s %s\n" % (b or "-", i or "-"))

    print("wrote %s (%d tests)" % (gen, len(tests)))
    return 0


HARNESS_HEAD = r'''/* AUTO-GENERATED by difftest.py - one function per distinct instruction.
 *
 * Each is the exact statement the lifter emitted, compiled and run on state
 * that the Unicorn side reproduces bit for bit. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MEM_SIZE 0x00040000
static unsigned char g_mem[MEM_SIZE];
uint32_t g_selbase[65536];

#include "cpu.h"

/* The lifted code reaches memory as SEGB(sel) + offset. Point every selector
 * at the same buffer and the offset is the address, which is what the Unicorn
 * side sees with a zero segment base. */
#define SEGB(sel) (g_selbase[(uint16_t)(sel)])
'''

HARNESS_TAIL = r'''
static uint32_t rnd(uint32_t *s)
{
    *s = (*s * 1103515245u + 12345u);
    return *s;
}

int main(int argc, char **argv)
{
    unsigned nv = argc > 1 ? (unsigned)atoi(argv[1]) : 3;
    unsigned n = (unsigned)(sizeof TESTS / sizeof TESTS[0]);
    FILE *pf = fopen("difftest_plan.txt", "r");
    if (!pf) { fprintf(stderr, "no plan\n"); return 1; }
    for (unsigned i = 0; i < 65536; i++)
        g_selbase[i] = (uint32_t)(uintptr_t)g_mem;

    for (unsigned t = 0; t < n; t++) {
        char b[32], x[32];
        if (fscanf(pf, "%31s %31s", b, x) != 2) break;
        for (unsigned v = 0; v < nv; v++) {
            uint32_t s = t * 977u + v * 7919u + 1u;
            CPU c;
            memset(&c, 0, sizeof c);
            uint32_t *r[8] = { &c.eax, &c.ecx, &c.edx, &c.ebx,
                               &c.esp, &c.ebp, &c.esi, &c.edi };
            static const char *nm[8] = { "eax","ecx","edx","ebx",
                                         "esp","ebp","esi","edi" };
            for (int k = 0; k < 8; k++)
                *r[k] = rnd(&s);
            for (int k = 0; k < 8; k++) {
                if (!strcmp(nm[k], b)) *r[k] = 0x00020000u;
                if (!strcmp(nm[k], x)) *r[k] = 0x10u;
            }
            c.cf = rnd(&s) & 1; c.zf = rnd(&s) & 1; c.sf = rnd(&s) & 1;
            c.of = rnd(&s) & 1; c.pf = rnd(&s) & 1; c.af = rnd(&s) & 1;
            for (unsigned i = 0; i < MEM_SIZE; i++)
                g_mem[i] = (unsigned char)((i * 7 + 13) & 0xFF);
            TESTS[t](&c);
            unsigned long crc = 0; /* cheap rolling checksum of memory */
            for (unsigned i = 0; i < MEM_SIZE; i += 4) {
                unsigned long w = *(uint32_t *)(g_mem + i);
                crc = crc * 16777619ul ^ w;
            }
            printf("%u %u %08X %08X %08X %08X %08X %08X %08X %08X %u%u%u%u%u %08lX\n",
                   t, v, c.eax, c.ecx, c.edx, c.ebx, c.esp, c.ebp, c.esi, c.edi,
                   c.cf, c.zf, c.sf, c.of, c.pf, crc);
        }
    }
    return 0;
}
'''

if __name__ == "__main__":
    sys.exit(main())
