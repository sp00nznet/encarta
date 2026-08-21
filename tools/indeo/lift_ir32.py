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


def segment_bitness(path, ne=None):
    """Which segments hold 32-bit code, decided by decoding them both ways.

    IR32.DLL is an NE - a 16-bit container - but the name is literal: the codec
    core inside is 32-bit. Nothing in the segment table says which is which. NE
    flag 0x1000 is DISCARDABLE, not USE32 (every segment here sets it), and
    there is no USE32 bit to read, so bitness has to be measured.

    Decoding a segment the wrong width does not fail loudly, it just produces
    junk: read as 16-bit, `mov eax, [edi+0x20c]` splits into `mov ax, [bx+0x20c]`
    plus `add [bx+si], al` on the high half. So decode each way and count the
    tells - bytes that decode to nothing, and all-zero instructions, which are
    what the top halves of 32-bit operands look like. The lower junk rate wins.

    On this DLL the split is decisive: segments 2, 3 and 13 come out 32-bit
    (segment 3 scores 0.176 as 16-bit against 0.062 as 32-bit), and the other 37
    come out 16-bit. Segment 1 is the useful counter-example - it sets the same
    flags as segment 3 but reads as 16-bit, and its `mov ax, [bp+6]` / `retf 4`
    argument handling confirms it.
    """
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32
    import ne_parse as _np
    if ne is None:
        ne = _np.parse_ne(path)
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    out = {}
    for seg in ne.code_segments:
        code = seg.data
        if not code:
            continue
        import decode16 as _d16
        n16 = bad16 = zero16 = pos = 0
        while pos < len(code):
            d = _d16.Decoder(code, 0); d.pos = pos
            try:
                ins = d.decode_one()
            except Exception:
                bad16 += 1; pos += 1; continue
            if not ins or ins.length <= 0:
                bad16 += 1; pos += 1; continue
            if code[pos:pos + ins.length] == bytes(ins.length):
                zero16 += 1
            n16 += 1; pos += ins.length
        n32 = bad32 = zero32 = pos = 0
        while pos < len(code):
            got = False
            for i in md.disasm(code[pos:], pos):
                if code[i.address:i.address + i.size] == bytes(i.size):
                    zero32 += 1
                n32 += 1; pos = i.address + i.size; got = True
            if not got:
                bad32 += 1; pos += 1
        j16 = (bad16 + zero16) / float(max(1, n16))
        j32 = (bad32 + zero32) / float(max(1, n32))
        out[seg.index] = j32 < j16
    return out


class _Op(object):
    """Minimal stand-in for decode16's operand, so descent works on both paths."""
    __slots__ = ("type", "disp")

    def __init__(self, type_, disp):
        self.type = type_
        self.disp = disp


class _Ins32(object):
    __slots__ = ("address", "length", "mnemonic", "op_str", "op1")

    def __init__(self, address, length, mnemonic, op_str, op1):
        self.address = address
        self.length = length
        self.mnemonic = mnemonic
        self.op_str = op_str
        self.op1 = op1

    def __repr__(self):
        return "%s %s" % (self.mnemonic, self.op_str)


def decode_segment32(code, decode16):
    """Linear sweep in 32-bit mode. Returns (instructions, undecoded_bytes).

    Emits objects shaped like decode16's, so the descent and carving below do
    not care which decoder produced them."""
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32
    from capstone.x86 import X86_OP_IMM, X86_OP_MEM
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    out, pos, bad, n = [], 0, 0, len(code)
    while pos < n:
        got = False
        for ins in md.disasm(code[pos:], pos):
            op1 = None
            ops = ins.operands or []
            if ops:
                o = ops[0]
                if o.type == X86_OP_IMM:
                    op1 = _Op(decode16.OpType.REL16, o.imm & 0xFFFFFFFF)
                elif o.type == X86_OP_MEM:
                    op1 = _Op(decode16.OpType.MEM, o.mem.disp & 0xFFFFFFFF)
            out.append(_Ins32(ins.address, ins.size, ins.mnemonic, ins.op_str, op1))
            pos = ins.address + ins.size
            got = True
        if not got:
            bad += 1
            pos += 1
    return out, bad


def far_call_entries(ne, is32):
    """Entry points recovered from far calls in every other segment.

    Segments here are entered by FAR call from elsewhere, so scanning one
    segment for near calls finds almost nothing - segment 1 yielded zero. The
    pair that names the callee is split across two places: a SELECTOR
    relocation names the target SEGMENT, and the call instruction's own
    immediate holds the OFFSET.

        9A 64 04 FF FF      call far <seg 9>:0x0464

    Two details matter. The `FF FF` is an end-of-chain marker, not a selector -
    NE chains several fixup sites per relocation record through the patched
    words, so following the chain finds 334 sites behind 112 records. And a
    SELECTOR fixup is not always a call: `B8/B9/BB imm16` is `mov reg,
    <selector>`, an ordinary data-segment load. Only sites preceded by 0x9A are
    entry points.
    """
    import struct
    code_segs = set(s.index for s in ne.code_segments)
    out = {}
    for seg in ne.segments:
        b = seg.data
        if not b:
            continue
        # a far call's offset is as wide as the segment making the call
        back, width = (5, 4) if is32.get(seg.index) else (3, 2)
        fmt = "<I" if width == 4 else "<H"
        for r in seg.relocations:
            if r.src_type != 2 or (r.flags & 3) != 0:
                continue
            if r.target_seg not in code_segs:
                continue
            o, seen = r.offset, set()
            while 0 <= o < len(b) - 1 and o not in seen:
                seen.add(o)
                nxt = struct.unpack_from("<H", b, o)[0]
                if o - back >= 0 and b[o - back] == 0x9A:
                    off = struct.unpack_from(fmt, b, o - width)[0]
                    out.setdefault(r.target_seg, set()).add(off)
                if nxt == 0xFFFF:
                    break
                o = nxt
    return out


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
              'jl','jge','jle','jg','jcxz','loop','loopz','loopnz',
              # capstone spellings, for the 32-bit path
              'jecxz','loope','loopne','jnae','jc','jnc','jna','jnbe','jpe','jpo',
              'jnge','jnl','jng','jnle'}
STOPS = {'ret', 'retf', 'iret', 'retn', 'iretd'}


def jump_table_targets(code, by_addr, disp, limit=256, esize=2):
    """Read a jump table at `disp` and return the targets it holds.

    Segment 3 dispatches through tables kept in the code segment itself:
        jmp word cs:[bx+si+0xDB8]
        table@0xDB8: 0DF0 0000 0E68 0000 1BB0 0000 ...
    Entries alternate with a zero high word, so the walk skips zeros and stops
    at the first non-zero value that is not a valid instruction address - which
    is where the table ends and something else begins."""
    import struct
    fmt = "<H" if esize == 2 else "<I"
    out, k, misses = [], 0, 0
    while k < limit and disp + esize * k + esize <= len(code):
        v = struct.unpack_from(fmt, code, disp + esize * k)[0]
        k += 1
        if v == 0 and esize == 2:
            # 16-bit reading of what is really a 32-bit table: skip high halves
            continue
        if v in by_addr:
            out.append(v); misses = 0
        else:
            misses += 1
            if misses >= 2:
                break
    return out


def trace(by_addr, start, decode16, code=None, esize=2, boundaries=()):
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
            if a != start and a in boundaries:
                # Another function starts here. Without this the walk runs
                # straight through the end of one function into the next, and
                # every entry reports nearly the whole segment as its body.
                break
            if code is not None and (code[a:a + 4] == bytes(4)
                                     or code[a] == 0xCC):
                # Inter-function padding: zero fill, or MSVC's int3 fill on the
                # 32-bit segments. Descent that walks into it lifts padding as
                # code, so stop and do not claim the address.
                break
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
                        work.extend(t for t in jump_table_targets(code, by_addr, d,
                                                                  esize=esize)
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


def carve_functions(decode16, instructions, seg_len, roots, seed_unreached=False,
                    code=None, esize=2):
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
    bounds = frozenset(roots)
    funcs, claimed, work = {}, set(), list(roots)
    while True:
        while work:
            r = work.pop(0)
            if r in claimed or r not in by_addr:
                continue
            body, calls = trace(by_addr, r, decode16, code=code,
                                esize=esize, boundaries=bounds)
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

    bitness = segment_bitness(a.ne_file, ne)
    is32 = bitness.get(a.seg, False)
    esize = 4 if is32 else 2
    if is32:
        insns, bad = decode_segment32(code, decode16)
    else:
        insns, bad = decode_segment(decode16, code)
    print("segment is %d-bit (decided by decoding it both ways)" % (32 if is32 else 16))
    print("decoded %d instructions, %d undecoded bytes (%.2f%% coverage)"
          % (len(insns), bad, 100.0 * (len(code) - bad) / max(1, len(code))))

    extra = set(int(x, 16) for x in a.entry)
    reloc_entries = entries_from_relocations(ne, a.seg)
    extra |= reloc_entries

    # exported entry points for this segment (DriverProc, LibMain, WEP, ...)
    exported = set(e.offset for e in (getattr(ne, "entries", None) or [])
                   if e.segment == a.seg)
    extra |= exported

    far_entries = far_call_entries(ne, bitness).get(a.seg, set())
    extra |= far_entries

    # `push bp; mov bp,sp` (55 8B EC) - a function that keeps a frame pointer
    # announces its own entry. Segment 3's codec inner loops are frameless and
    # have none, which is why descent alone was the only option there, but
    # segment 1 has 58 of them and near-call scanning finds almost nothing.
    # Only counted at an address the sweep already decodes as an instruction,
    # so the byte pattern occurring inside a longer instruction is ignored.
    decoded_at = set(i.address for i in insns)
    prologues = set(o for o in range(len(code) - 2)
                    if code[o] == 0x55 and code[o + 1] == 0x8B and code[o + 2] == 0xEC
                    and o in decoded_at)
    extra |= prologues
    print("entries: %d far-call, %d exported, %d selector-reloc, %d prologue"
          % (len(far_entries), len(exported), len(reloc_entries), len(prologues)))

    by_all = {i.address: i for i in insns}

    # Jump tables in code descent has NOT verified, accepted only when the table
    # sits just past the jump that reads it:
    #
    #     jmp word cs:[bx+si+0xDB8]   at 0x0DB0, 5 bytes -> table at 0x0DB8
    #
    # Measured across segment 3 the gap is consistently +3 to +5 (alignment
    # padding), never 0. That is the compiler's switch idiom, and it separates a
    # real dispatch from a misaligned byte pair that merely decodes as
    # `jmp [mem]`. Harvesting without the test pulled in 26 targets that a later
    # fixpoint pass showed lay in no verified code at all - noise, and every one
    # would have become a fake function. Two jumps here point at tables ~1 KB
    # BEHIND them; those stay rejected until something corroborates them.
    inline_jt = set()
    for ins in insns:
        if (ins.mnemonic or '').lower() != 'jmp' or ins.op1 is None:
            continue
        if getattr(ins.op1, 'type', None) != decode16.OpType.MEM:
            continue
        d = getattr(ins.op1, 'disp', 0)
        gap = d - (ins.address + ins.length)
        if d and 0 <= gap <= 8 and d < len(code):
            inline_jt.update(jump_table_targets(code, by_all, d, esize=esize))
    if inline_jt:
        print("inline jump tables: %d targets (table directly after the jump)"
              % len(inline_jt))
    extra |= inline_jt
    starts = find_function_starts(decode16, insns, len(code), extra)
    print("function starts: %d (%d from relocations, %d from near calls)"
          % (len(starts), len(reloc_entries), len(starts) - len(reloc_entries & set(starts)) - 1))

    # carve functions by following control flow, not by cutting the linear
    # sweep at known starts - frameless code has no prologue to cut at
    # Descent and jump-table harvesting feed each other, so iterate to a
    # fixpoint rather than doing either once.
    #
    # Harvesting from the whole linear sweep is tempting and wrong: a linear
    # sweep is misaligned wherever it runs through data, and a misaligned byte
    # pair reads as `jmp [mem]` happily enough - its "table" is then noise, and
    # every address it yields becomes a fake function. Only code that descent
    # has actually walked is known to be aligned, so tables are read from that,
    # which then reaches more code, which may hold more tables.
    rounds, tried = 0, set(starts)
    while True:
        rounds += 1
        fmap, claimed, by_addr = carve_functions(decode16, insns, len(code), starts,
                                                 seed_unreached=a.seed_unreached,
                                                 code=code, esize=esize)
        found = set()
        for addr in claimed:
            ins = by_addr.get(addr)
            if ins is None or (ins.mnemonic or '').lower() != 'jmp' or ins.op1 is None:
                continue
            if getattr(ins.op1, 'type', None) != decode16.OpType.MEM:
                continue
            d = getattr(ins.op1, 'disp', 0)
            if 0 < d < len(code):
                found.update(jump_table_targets(code, by_addr, d, esize=esize))
        fresh = {t for t in found if t not in claimed and t in by_addr
                 and t not in tried}
        tried |= fresh
        if not fresh or rounds > 12:
            break
        print("  round %d: %d jump-table targets in verified code -> %d new roots"
              % (rounds, len(found), len(fresh)))
        starts = sorted(set(starts) | fresh)
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

        # Does what we carved actually look like code? Recovering a fake entry
        # point is not a loud failure - the lifter emits confident C for it all
        # the same - so check rather than assume. Two tells give it away:
        # zero-fill (`add [bx+si], al`, bytes 00 00) and opcodes a compiler does
        # not emit in a codec inner loop (int, into, aam/aad, salc).
        JUNK = {'int', 'int3', 'into', 'aam', 'aad', 'salc', 'icebp',
                '?', '???', 'db', 'bad', 'invalid'}
        flagged = []
        for start, body in funcs:
            zeros = sum(1 for i in body
                        if code[i.address:i.address + i.length] == bytes(i.length))
            junk = sum(1 for i in body if (i.mnemonic or '?').lower() in JUNK)
            frac = (zeros + junk) / float(len(body))
            if frac > 0.05:
                flagged.append((frac, start, len(body), zeros, junk))
        if flagged:
            print("suspect functions (>5%% zero-fill or junk opcodes):")
            for frac, start, n, z, j in sorted(flagged, reverse=True):
                print("   0x%04X  %3d insns  %.0f%% junk (%d zero-fill, %d bad opcode)"
                      % (start, n, frac * 100, z, j))
            print("   ^ these entry points are probably not real code")
        else:
            print("no function shows zero-fill or junk-opcode density: carve looks clean")
        return 0

    # 32-bit segments go through pcrecomp's 32-bit lifter - the same one that
    # produced the Encarta executable - not lift16. It is written against a PE,
    # but only loosely: `read_va` is injectable and the image bounds are plain
    # attributes, so a 64K NE segment can pose as a tiny image based at 0.
    parts, unhandled = [], 0
    if is32:
        import lift32_cpu

        class _NELifter(lift32_cpu.Lifter):
            def disp_is_addr(self, insn, d):
                """In an NE segment a displacement is never an image address.

                The base class decides this from the PE .reloc table and falls
                back to a range check, which here wraps every small constant:
                `lea edi, [edi+4]` came out as `edi + GVA(4)`. A segment is its
                own 0..64K space and its data lives in other segments reached
                through DS, so no displacement wants wrapping."""
                return False

        lifter = _NELifter(a.ne_file, len(code),
                           read_va=lambda va, n: code[va:va + n])
        lifter.image_lo, lifter.image_hi = 0, len(code)
        emitted = set()
        for start, body in funcs:
            # Name and slice from the ENTRY, not from the lowest address the
            # body reached. Descent follows backward jumps, so body[0] can sit
            # far below the entry - keying on it emitted one shared region 16
            # times under the same name, and the file would not have compiled.
            lo = start
            hi = max(i.address + i.length for i in body)
            if hi <= lo or (lo, hi) in emitted:
                continue
            emitted.add((lo, hi))
            try:
                c = lifter.lift_function(code[lo:hi], lo)
            except Exception as e:
                print("  !! %04X failed: %s: %s" % (start, type(e).__name__, e))
                continue
            if isinstance(c, list):
                c = "\n".join(c)
            unhandled += c.count("UNHANDLED")
            parts.append(c)
    else:
        lifter = lift16.Lifter()
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
                " * %s NE code lifted via pcrecomp %s.\n */\n"
                '#include "%s"\n\n'
                % (a.seg,
                   "32-bit" if is32 else "16-bit",
                   "capstone + lift32_cpu" if is32 else "decode16 + lift16",
                   "recomp32.h" if is32 else "recomp16.h"))
        if is32:
            f.write(
                "/* NOTE: GVA() must be the identity here. The 32-bit\n"
                " * lifter is written against a PE, where a jump table\n"
                " * holds virtual addresses; an NE segment's table holds\n"
                " * plain segment offsets, so wrapping them would make\n"
                " * every switch miss. There is no NE 32-bit runtime yet:\n"
                " * this C is the lift result, it does not compile as-is.\n"
                " */\n\n")

        f.write("\n\n".join(parts))
        f.write("\n")
    print("wrote %s: %d functions, %d lines, %d UNHANDLED instructions"
          % (out, len(parts), sum(p.count("\n") for p in parts), unhandled))
    return 0


if __name__ == "__main__":
    sys.exit(main())
