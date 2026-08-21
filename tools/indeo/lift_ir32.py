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


COND_JUMPS = {'jo','jno','jb','jae','je','jne','jbe','ja','js','jns','jp','jnp',
              'jl','jge','jle','jg','jcxz','loop','loopz','loopnz'}
STOPS = {'ret', 'retf', 'iret', 'retn'}


def jump_table_targets(code, by_addr, disp, limit=256):
    """Read a jump table at `disp` and return the targets it holds.

    Segment 3 dispatches through tables kept in the code segment itself:
        jmp word cs:[bx+si+0xDB8]
        table@0xDB8: 0DF0 0000 0E68 0000 1BB0 0000 ...
    Entries alternate with a zero high word, so the walk skips zeros and stops
    at the first non-zero value that is not a valid instruction address - which
    is where the table ends and something else begins."""
    import struct
    out, k, misses = [], 0, 0
    while k < limit and disp + 2 * k + 1 < len(code):
        v = struct.unpack_from("<H", code, disp + 2 * k)[0]
        k += 1
        if v == 0:
            continue
        if v in by_addr:
            out.append(v); misses = 0
        else:
            misses += 1
            if misses >= 2:
                break
    return out


def trace(by_addr, start, decode16, code=None):
    """Recursive descent from one entry. Returns (addresses in this function,
    near-call targets it makes).

    Segment 3 has 95 returns and not one `push bp; mov bp,sp` - optimised codec
    code keeps no frame pointer - so prologue scanning finds nothing and the
    whole segment lifts as a single blob. Following control flow finds the real
    extent instead: walk from an entry, branch at conditionals, stop at a
    return or an indirect/far transfer."""
    body, calls, work = set(), set(), [start]
    while work:
        a = work.pop()
        while a in by_addr and a not in body:
            body.add(a)
            ins = by_addr[a]
            m = (ins.mnemonic or '').lower()
            op1 = ins.op1
            rel = op1 is not None and getattr(op1, 'type', None) in (
                decode16.OpType.REL8, decode16.OpType.REL16)
            if m in STOPS:
                break
            if m == 'jmp':
                if rel and op1.disp in by_addr:
                    a = op1.disp          # keep walking, same function
                    continue
                if (code is not None and op1 is not None
                        and getattr(op1, 'type', None) == decode16.OpType.MEM):
                    # switch dispatch: the cases belong to this function
                    d = getattr(op1, 'disp', 0)
                    if 0 < d < len(code):
                        work.extend(t for t in jump_table_targets(code, by_addr, d)
                                    if t not in body)
                break                     # indirect or far: flow leaves us
            if m in COND_JUMPS:
                if rel and op1.disp in by_addr:
                    work.append(op1.disp)
                a += ins.length
                continue
            if m == 'call':
                if rel:
                    calls.add(op1.disp)   # a separate function
                a += ins.length
                continue
            a += ins.length
    return body, calls


def carve_functions(decode16, instructions, seg_len, roots, seed_unreached=False, code=None):
    """Recursive descent from every known entry.

    By default it stops there and reports what it could not reach. Seeding new
    roots into the leftovers reaches 100% "coverage", but a code segment holds
    tables too, and disassembling those manufactures convincing functions out
    of nothing - a run of 00 bytes becomes `add [bx+si], al` and an `int 0x0`.
    Segment 3 carves into 3 real functions and 142 imaginary ones that way. A
    number that only looks good is worse than a small honest one, so seeding is
    opt-in."""
    by_addr = {ins.address: ins for ins in instructions}
    ordered = sorted(by_addr)
    funcs, claimed, work = {}, set(), list(roots)
    while True:
        while work:
            r = work.pop(0)
            if r in claimed or r not in by_addr:
                continue
            body, calls = trace(by_addr, r, decode16, code)
            if not body:
                continue
            funcs[r] = body
            claimed |= body
            for c in calls:
                if c not in funcs and c in by_addr:
                    work.append(c)
        # Nothing reachable left. Restart at the first unclaimed instruction,
        # which is usually the head of a function only ever entered indirectly.
        # (Doing this in the inner loop skipped seeding whenever a root was
        # already claimed, and the whole carve terminated early.)
        if not seed_unreached:
            break
        nxt = next((a for a in ordered if a not in claimed), None)
        if nxt is None:
            break
        work.append(nxt)
    return funcs, claimed, by_addr


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
    ap.add_argument("--seed-unreached", action="store_true",
                    help="also lift code no entry point reaches (will lift data as code)")
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

    # Jump tables have to be harvested statically, not during descent: they sit
    # INSIDE the functions we cannot reach, so waiting until control flow gets
    # there never happens. Scan every `jmp [mem]` with a constant displacement
    # and take everything its table points at.
    by_all = {i.address: i for i in insns}
    jt = set()
    for ins in insns:
        if (ins.mnemonic or '').lower() != 'jmp' or ins.op1 is None:
            continue
        if getattr(ins.op1, 'type', None) != decode16.OpType.MEM:
            continue
        d = getattr(ins.op1, 'disp', 0)
        if 0 < d < len(code):
            jt.update(jump_table_targets(code, by_all, d))
    if jt:
        print("jump tables: %d targets recovered from %d indirect jumps"
              % (len(jt), sum(1 for i in insns
                              if (i.mnemonic or '').lower() == 'jmp' and i.op1 is not None
                              and getattr(i.op1, 'type', None) == decode16.OpType.MEM)))
    extra |= jt
    starts = find_function_starts(decode16, insns, len(code), extra)
    print("function starts: %d (%d from relocations, %d from near calls)"
          % (len(starts), len(reloc_entries), len(starts) - len(reloc_entries & set(starts)) - 1))

    # carve functions by following control flow, not by cutting the linear
    # sweep at known starts - frameless code has no prologue to cut at
    fmap, claimed, by_addr = carve_functions(decode16, insns, len(code), starts,
                                             seed_unreached=a.seed_unreached, code=code)
    funcs = []
    for start in sorted(fmap):
        body = [by_addr[a] for a in sorted(fmap[start])]
        if body:
            funcs.append((start, body))
    pct = 100.0 * len(claimed) / max(1, len(insns))
    print("carved %d functions from %d instructions reachable of %d (%.1f%%)"
          % (len(funcs), len(claimed), len(insns), pct))
    if not a.seed_unreached and pct < 90.0:
        print("   %d instructions unreached - tables, or entry points we have not"
              % (len(insns) - len(claimed)))
        print("   found yet (indirect calls). Not lifted; --seed-unreached forces it.")

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
