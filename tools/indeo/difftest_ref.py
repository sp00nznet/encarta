"""The reference half of difftest: run each instruction on real x86 semantics.

Same state as the C harness, byte for byte - same pseudorandom generator, same
register plan, same memory pattern - so any difference in the output is a
difference in what the instruction MEANS, not in what it was given.
"""
import argparse
import os
import re
import struct
import sys

MEM_SIZE = 0x00040000
GPR = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]


def rnd(state):
    state[0] = (state[0] * 1103515245 + 12345) & 0xFFFFFFFF
    return state[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ne_file")
    ap.add_argument("--seg", type=int, default=3)
    ap.add_argument("--pcrecomp", default=r"G:\recomp\pc\tools")
    ap.add_argument("--build", required=True)
    ap.add_argument("--vectors", type=int, default=3)
    a = ap.parse_args()

    sys.path.insert(0, os.path.join(a.pcrecomp, "tools", "ne"))
    import ne_parse
    import capstone
    from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_PROT_ALL
    from unicorn import x86_const as X

    ne = ne_parse.parse_ne(a.ne_file)
    seg = [s for s in ne.code_segments if s.index == a.seg][0]
    code = bytes(seg.data)

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True

    # rebuild the identical test list difftest.py produced
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import difftest
    seen, tests = set(), []
    for addr, stmt, text in difftest.statements(
            os.path.join(a.build, "ir32_seg%d.c" % a.seg)):
        if addr >= len(code):
            continue
        try:
            insn = next(md.disasm(code[addr:addr + 16], addr))
        except StopIteration:
            continue
        raw = bytes(insn.bytes)
        if raw in seen or not difftest.testable(stmt, text):
            continue
        seen.add(raw)
        base = idx = None
        for op in insn.operands:
            if op.type == capstone.x86.X86_OP_MEM:
                if op.mem.base:
                    base = md.reg_name(op.mem.base)
                if op.mem.index:
                    idx = md.reg_name(op.mem.index)
        tests.append((raw, text, base, idx))

    REG = {n: getattr(X, "UC_X86_REG_" + n.upper()) for n in GPR}
    CODE_AT = 0x00300000
    pattern = bytes(((i * 7 + 13) & 0xFF) for i in range(MEM_SIZE))

    out = []
    for t, (raw, text, base, idx) in enumerate(tests):
        for v in range(a.vectors):
            st = [(t * 977 + v * 7919 + 1) & 0xFFFFFFFF]
            regs = [rnd(st) for _ in range(8)]
            vals = dict(zip(GPR, regs))
            if base:
                vals[base] = 0x00020000
            if idx:
                vals[idx] = 0x10
            cf, zf, sf, of, pf, af = (rnd(st) & 1 for _ in range(6))

            uc = Uc(UC_ARCH_X86, UC_MODE_32)
            uc.mem_map(0, MEM_SIZE, UC_PROT_ALL)
            uc.mem_map(CODE_AT, 0x1000, UC_PROT_ALL)
            uc.mem_write(0, pattern)
            uc.mem_write(CODE_AT, raw + b"\xf4")     # hlt, so it stops cleanly
            for n in GPR:
                uc.reg_write(REG[n], vals[n])
            fl = 0x2 | (cf << 0) | (pf << 2) | (af << 4) | \
                 (zf << 6) | (sf << 7) | (of << 11)
            uc.reg_write(X.UC_X86_REG_EFLAGS, fl)
            try:
                uc.emu_start(CODE_AT, CODE_AT + len(raw), count=1)
            except Exception as e:
                out.append((t, v, None, str(e)))
                continue
            got = [uc.reg_read(REG[n]) for n in GPR]
            e = uc.reg_read(X.UC_X86_REG_EFLAGS)
            fo = ((e >> 0) & 1, (e >> 6) & 1, (e >> 7) & 1,
                  (e >> 11) & 1, (e >> 2) & 1)
            mem = uc.mem_read(0, MEM_SIZE)
            crc = 0
            for off in range(0, MEM_SIZE, 4):
                w = struct.unpack_from("<I", mem, off)[0]
                crc = ((crc * 16777619) ^ w) & 0xFFFFFFFFFFFFFFFF
            out.append((t, v, (got, fo, crc & 0xFFFFFFFF), text))

    with open(os.path.join(a.build, "difftest_ref.txt"), "w") as f:
        for t, v, r, text in out:
            if r is None:
                f.write("%u %u ERROR %s\n" % (t, v, text))
                continue
            got, fo, crc = r
            f.write("%u %u %s %s %08X\n"
                    % (t, v, " ".join("%08X" % x for x in got),
                       "".join(str(x) for x in fo), crc))
    print("wrote difftest_ref.txt: %d rows over %d instructions"
          % (len(out), len(tests)))


if __name__ == "__main__":
    main()
