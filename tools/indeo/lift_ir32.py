#!/usr/bin/env python3
"""
lift_ir32.py - lift a 16-bit NE code segment to C, using pcrecomp's x86-16
               decoder and lifter.

IR32.DLL is the Indeo 3 decoder and is a 16-bit NE binary, so it cannot be
loaded in-process the way DECO_32.DLL was. Lifting it is the same play that
produced a byte-exact DECO_32 - this is the driver that feeds NE segments
through `decode16` and `lift16`, which have no NE front end of their own.

Addressing: an NE segment is its own 0..64K space, so instruction addresses
here are segment-relative and near control flow resolves inside one C file per
segment. Far calls leave the segment and become recomp_dispatch(), which the
runtime resolves.

Function starts are recovered from near-call targets - E8 rel16 within the
segment - plus offset 0. That finds every function reached by a near call from
the same segment; anything only ever entered by a far call from elsewhere needs
the caller's relocations, which -entry can supply.

Usage:
    py lift_ir32.py <ne-file> --seg 3 -o ir32_seg3.c
    py lift_ir32.py <ne-file> --seg 3 --stats
"""
import sys, os, argparse


def load_tools(pcrecomp):
    sys.path.insert(0, os.path.join(pcrecomp, "tools", "disasm"))
    sys.path.insert(0, os.path.join(pcrecomp, "tools", "ne"))
    sys.path.insert(0, os.path.join(pcrecomp, "tools", "lift"))
    import decode16, ne_parse, lift16
    return decode16, ne_parse, lift16


def decode_segment(decode16, code):
    """Linear sweep. Returns (instructions, undecoded_byte_count)."""
    out, pos, bad = [], 0, 0
    while pos < len(code):
        d = decode16.Decoder(code, 0)
        d.pos = pos
        try:
            ins = d.decode_one()
        except Exception:
            bad += 1; pos += 1; continue
        if ins is None or ins.length <= 0:
            bad += 1; pos += 1; continue
        if (ins.mnemonic or "").lower() == "db":
            bad += ins.length
        out.append(ins)
        pos += ins.length
    return out, bad


def entries_from_relocations(ne, seg_index):
    """Offsets in this segment that other segments far-call.

    A leaf segment like IR32's compute code is entered only by FAR calls, so
    near-call scanning finds nothing there. Recovering those targets needs one
    NE detail: every internal relocation in this DLL is a SELECTOR fixup, which
    patches only the segment half of the pointer - `target_off` is not an entry
    point and reading it as one finds almost nothing.

    The offset is an immediate in the instruction instead. 92 of the 112
    internal relocations sit exactly three bytes past a `9A` (far call direct)
    opcode, so the call is `9A off16 seg16` with the fixup on seg16: the entry
    point is the off16 immediately before it."""
    import struct
    hits = set()
    for s in ne.segments:
        d = s.data
        for r in s.relocations:
            if (r.flags & 3) != 0 or r.target_seg != seg_index:
                continue
            o = r.offset
            if o >= 3 and o + 2 <= len(d) and d[o - 3] in (0x9A, 0xEA):
                # `9A off16 seg16` - direct far call, offset precedes the fixup
                hits.add(struct.unpack_from("<H", d, o - 2)[0])
            elif o >= 5 and d[o - 3] == 0xC7 and d[o - 2] == 0x46 and d[o - 8] == 0xC7:
                # A far pointer assembled in locals, which is how the compute
                # segments are reached:
                #     C7 46 e6 40 4F   mov [bp-1A], 0x4F40   <- offset
                #     C7 46 e8 D5 0B   mov [bp-18], seg      <- fixup lands here
                # The offset is the immediate of the PREVIOUS store.
                hits.add(struct.unpack_from("<H", d, o - 5)[0])
            elif o >= 2 and d[o - 1] in (0xB8, 0xBA, 0xB9, 0xBB):
                # `mov reg, seg` with the offset pushed separately - the fixup
                # gives us the segment only, so fall back to the entry table.
                pass
    return hits


def find_function_starts(decode16, instructions, seg_len, extra):
    """Offset 0, any near-call target inside the segment, and anything the
    caller passed in (relocation-derived entries or -entry)."""
    starts = {0}
    for e in extra:
        if 0 <= e < seg_len:
            starts.add(e)
    for ins in instructions:
        if ins.mnemonic == "call" and ins.op1 is not None:
            t = getattr(ins.op1, "type", None)
            if t in (decode16.OpType.REL8, decode16.OpType.REL16):
                tgt = ins.op1.disp
                if 0 <= tgt < seg_len:
                    starts.add(tgt)
    return sorted(starts)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ne_file")
    ap.add_argument("--pcrecomp", default=r"G:\recomp\pc\tools")
    ap.add_argument("--seg", type=int, required=True)
    ap.add_argument("-o", "--out")
    ap.add_argument("--stats", action="store_true")
    ap.add_argument("--entry", action="append", default=[],
                    help="extra function start, hex offset (repeatable)")
    a = ap.parse_args()

    decode16, ne_parse, lift16 = load_tools(a.pcrecomp)
    ne = ne_parse.parse_ne(a.ne_file)
    segs = [s for s in ne.code_segments if s.index == a.seg]
    if not segs:
        print("no code segment %d" % a.seg); return 2
    seg = segs[0]
    code = seg.data
    print("segment %d: %d bytes, %d relocations" % (a.seg, len(code), len(seg.relocations)))

    insns, bad = decode_segment(decode16, code)
    print("decoded %d instructions, %d undecoded bytes (%.2f%% coverage)"
          % (len(insns), bad, 100.0 * (len(code) - bad) / max(1, len(code))))

    extra = set(int(x, 16) for x in a.entry)
    reloc_entries = entries_from_relocations(ne, a.seg)
    extra |= reloc_entries
    starts = find_function_starts(decode16, insns, len(code), extra)
    print("function starts: %d (%d from relocations, %d from near calls)"
          % (len(starts), len(reloc_entries), len(starts) - len(reloc_entries & set(starts)) - 1))

    # split the instruction stream at function starts
    by_addr = {}
    for ins in insns:
        by_addr.setdefault(ins.address, ins)
    funcs = []
    for i, s in enumerate(starts):
        end = starts[i + 1] if i + 1 < len(starts) else len(code)
        body = [ins for ins in insns if s <= ins.address < end]
        if body:
            funcs.append((s, body))

    if a.stats:
        sizes = sorted((len(b) for _, b in funcs), reverse=True)
        print("functions: %d  largest: %s  median: %d"
              % (len(funcs), sizes[:5], sizes[len(sizes) // 2] if sizes else 0))
        return 0

    lifter = lift16.Lifter()
    parts, unhandled = [], 0
    for start, body in funcs:
        name = "ir32_s%d_%04X" % (a.seg, start)
        try:
            c = lifter.lift_function(name, body, start)
        except Exception as e:
            print("  !! %s failed: %s: %s" % (name, type(e).__name__, e))
            continue
        if isinstance(c, list):
            c = "\n".join(c)
        unhandled += c.count("UNHANDLED")
        parts.append(c)

    out = a.out or ("ir32_seg%d.c" % a.seg)
    with open(out, "w") as f:
        f.write("/* AUTO-GENERATED by lift_ir32.py from IR32.DLL segment %d.\n"
                " * 16-bit NE code lifted via pcrecomp decode16 + lift16.\n */\n"
                '#include "recomp16.h"\n\n' % a.seg)
        f.write("\n\n".join(parts))
        f.write("\n")
    print("wrote %s: %d functions, %d lines, %d UNHANDLED instructions"
          % (out, len(parts), sum(p.count("\n") for p in parts), unhandled))
    return 0


if __name__ == "__main__":
    sys.exit(main())
