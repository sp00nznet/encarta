#!/usr/bin/env python3
"""
ir32_scan.py - decode coverage survey of IR32.DLL's 16-bit code segments.

Before lifting a binary it is worth knowing whether the decoder understands it.
This runs a linear sweep over every code segment of an NE file and reports what
fraction decodes and which opcodes it chokes on - the same first question the
DECO_32 recompilation asked ("0 unhandled opcodes") before any C was emitted.

Linear sweep will mis-align on data embedded in code, so a small tail of
failures is expected and is not the same thing as an unsupported instruction.

Usage:
    py ir32_scan.py <ne-file> [--pcrecomp <path-to-pcrecomp>] [--seg N]
"""
import sys, os, argparse, collections

# prefix bytes: seg overrides, operand/addr size, lock/rep, and FWAIT
PREFIXES = {0x26,0x2E,0x36,0x3E,0x64,0x65,0x66,0x67,0xF0,0xF2,0xF3,0x9B}


def load_tools(pcrecomp):
    sys.path.insert(0, os.path.join(pcrecomp, "tools", "disasm"))
    sys.path.insert(0, os.path.join(pcrecomp, "tools", "ne"))
    import decode16, ne_parse
    return decode16, ne_parse


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ne_file")
    ap.add_argument("--pcrecomp", default=r"G:\recomp\pc\tools")
    ap.add_argument("--seg", type=int, default=0, help="only this segment")
    a = ap.parse_args()

    decode16, ne_parse = load_tools(a.pcrecomp)
    ne = ne_parse.parse_ne(a.ne_file)
    data = open(a.ne_file, "rb").read()
    total_bytes = ok_bytes = 0
    total_insns = 0
    failures = collections.Counter()
    per_seg = []

    for seg in ne.code_segments:
        i = seg.index
        if a.seg and i != a.seg:
            continue
        code = seg.data if seg.data else data[seg.file_offset:seg.file_offset + seg.actual_size]
        if not code:
            continue

        dec = decode16.Decoder(code, 0)
        pos = n_ins = good = 0
        while pos < len(code):
            dec.pos = pos
            try:
                ins = dec.decode_one()
            except Exception as e:
                failures["exception: %s" % type(e).__name__] += 1
                pos += 1
                continue
            if ins is None or getattr(ins, "length", 0) <= 0:
                failures["undecodable byte %02X" % code[pos]] += 1
                pos += 1
                continue
            m = (getattr(ins, "mnemonic", "") or "").lower()
            if m in ("", "???", "bad", "invalid", "db"):
                # Report the OPCODE, not the first byte: an instruction that
                # starts with a prefix would otherwise be blamed on the prefix
                # and make a handled prefix look unsupported.
                q = pos
                while q < len(code) and code[q] in PREFIXES:
                    q += 1
                if q < len(code) and code[q] == 0x0F and q + 1 < len(code):
                    failures["0F %02X" % code[q + 1]] += 1
                elif q < len(code):
                    failures["%02X" % code[q]] += 1
                pos += ins.length
                continue
            good += ins.length
            n_ins += 1
            pos += ins.length

        total_bytes += len(code); ok_bytes += good; total_insns += n_ins
        per_seg.append((i, len(code), good, n_ins))

    print("=== decode coverage: %s ===" % os.path.basename(a.ne_file))
    print("%-6s %-10s %-10s %-8s %s" % ("seg", "bytes", "decoded", "insns", "coverage"))
    for i, sz, good, n in per_seg:
        print("%-6d %-10d %-10d %-8d %.1f%%" % (i, sz, good, n, 100.0 * good / sz if sz else 0))
    print("-" * 52)
    print("TOTAL  %-10d %-10d %-8d %.2f%%" %
          (total_bytes, ok_bytes, total_insns,
           100.0 * ok_bytes / total_bytes if total_bytes else 0))
    if failures:
        print("\ntop decode failures:")
        for k, v in failures.most_common(12):
            print("   %-28s x%d" % (k, v))
    return 0


if __name__ == "__main__":
    sys.exit(main())
