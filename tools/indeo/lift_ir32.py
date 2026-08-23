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


def apply_selector_fixups(ne, seg_index, code):
    """Patch a code segment's selector fixups before it is lifted.

    This has to happen here rather than in the runtime, and the reason is easy
    to get wrong. A loader patches the image and the CPU then executes the
    patched bytes. Lifting compiles the bytes into C, so an immediate becomes a
    constant: `mov eax, 0xFFFF` lifts to `c->eax = 0xFFFFu;` and no amount of
    patching the loaded segment afterwards will change it.

    The init routine at 3:0000 does exactly that - `mov eax, 0xFFFF / mov ds,
    ax` - and 0xFFFF is not a selector, it is the end-of-chain marker sitting
    in an unpatched fixup slot. Lifted unpatched, DS becomes a selector nothing
    is mapped at.

    NE chains its fixup sites: the word at one holds the offset of the next
    taking the same value. Read the link before overwriting it.

    Selector == NE segment number, which is a convention the runtime's ne_map
    has to agree with, and does.
    """
    import struct
    out = bytearray(code)
    segs = [x for x in ne.segments if x.index == seg_index]
    if not segs:
        return bytes(out), 0
    n = 0
    for r in segs[0].relocations:
        kind = r.flags & 3
        if kind in (1, 2):
            # An import. Give it a synthetic selector - 0xF000 | module index -
            # and put the ordinal where the offset goes, so a call to it lifts
            # into an ordinary far call that the runtime can name. Left
            # unpatched these read as a call to 0000:FFFF, which is the chain
            # terminator rather than an address, and tells nobody anything.
            if r.src_type not in (3, 11):        # FAR_PTR / PTR48
                continue
            off, guard = r.offset, 0
            sel = 0xF000 | (r.module_idx & 0xFF)
            while 0 <= off + 3 < len(out) and guard < 4096:
                guard += 1
                nxt = struct.unpack_from("<H", out, off)[0]
                struct.pack_into("<H", out, off, r.ordinal & 0xFFFF)
                struct.pack_into("<H", out, off + 2, sel)
                n += 1
                if nxt == 0xFFFF:
                    break
                off = nxt
            continue
        if r.src_type != 2 or kind != 0:
            continue
        off, guard = r.offset, 0
        while 0 <= off + 1 < len(out) and guard < 4096:
            guard += 1
            nxt = struct.unpack_from("<H", out, off)[0]
            struct.pack_into("<H", out, off, r.target_seg & 0xFFFF)
            n += 1
            if nxt == 0xFFFF:
                break
            off = nxt
    return bytes(out), n


def make_validator(code, is32, decode16):
    """Does an address look like the start of real instructions?

    Membership in the linear sweep is the obvious test and the wrong one: a
    sweep is misaligned wherever it crosses a table, so a perfectly good jump
    target need not be one of its instruction boundaries. That is what hid the
    first two entries of the table at 0x1C3C. Decode AT the address instead."""
    cache = {}
    if is32:
        from capstone import Cs, CS_ARCH_X86, CS_MODE_32
        md = Cs(CS_ARCH_X86, CS_MODE_32)

        def probe(v):
            n = b = 0
            for ins in md.disasm(code[v:v + 32], v):
                n += 1
                b += ins.size
                if n >= 3:
                    break
            return n >= 3 and b >= 6
    else:
        def probe(v):
            n = b = pos = 0
            while n < 3 and pos < 32 and v + pos < len(code):
                d = decode16.Decoder(code, 0)
                d.pos = v + pos
                try:
                    ins = d.decode_one()
                except Exception:
                    return False
                if not ins or ins.length <= 0:
                    return False
                n += 1
                b += ins.length
                pos += ins.length
            return n >= 3 and b >= 6

    def valid(v):
        if v not in cache:
            cache[v] = bool(0 < v < len(code)) and probe(v)
        return cache[v]
    return valid


def table_after_jump(code, ins, esize, valid, maxpad=16, limit=64):
    """The jump table an indirect jump reads, which follows the jump itself.

    The displacement is not where the table is. In segment 3

        0x1C32  jmp dword ptr cs:[edx*4 + 0x185C]

    reads a table at 0x1C3C - eight bytes of instruction, two bytes of
    `mov eax, eax` alignment padding, then the entries. The displacement is a
    base the loader fixes up elsewhere and means nothing to us; the table's
    real location is "just past the jump, aligned". So scan forward for the
    aligned offset that begins the longest run of valid targets.
    """
    end = ins.address + ins.length
    best = []
    for pad in range(maxpad + 1):
        off = end + pad
        if off % esize:
            continue
        run = []
        for k in range(limit):
            q = off + esize * k
            if q + esize > len(code):
                break
            v = int.from_bytes(code[q:q + esize], "little")
            if valid(v):
                run.append(v)
            else:
                break
        if len(run) > len(best):
            best = run
    return best if len(best) >= 2 else []


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


def trace(by_addr, start, decode16, code=None, esize=2, boundaries=(),
          valid=None):
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
                if (code is not None and valid is not None and op1 is not None
                        and getattr(op1, 'type', None) == decode16.OpType.MEM):
                    # switch dispatch: the cases belong to this function
                    work.extend(t for t in table_after_jump(code, ins, esize, valid)
                                if t not in body and t in by_addr)
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


def resync(code, by_addr, start, esize, decode16, limit=4096):
    """Re-decode from a known entry point and splice it into the sweep.

    A linear sweep is misaligned wherever it runs through a table, and stays
    misaligned until it happens to resync. Every entry we recover - a thunk
    prologue, a jump-table target, a far-call offset - is better evidence of an
    instruction boundary than the sweep is, but `carve_functions` used to drop
    any root the sweep did not already know, so seeding 0x2800 as a root did
    nothing at all: it was not one of the sweep's boundaries, so it was
    discarded and its whole function stayed unreached.

    Decode forward from the entry instead, adding instructions the sweep does
    not have, and stop on rejoining it - from there the sweep is right again.
    """
    added = 0
    if esize == 4:
        from capstone import Cs, CS_ARCH_X86, CS_MODE_32
        md = Cs(CS_ARCH_X86, CS_MODE_32)
        md.detail = True
        from capstone.x86 import X86_OP_IMM, X86_OP_MEM
        for ins in md.disasm(code[start:start + limit], start):
            if ins.address in by_addr:
                break
            op1 = None
            ops = ins.operands or []
            if ops:
                o = ops[0]
                if o.type == X86_OP_IMM:
                    op1 = _Op(decode16.OpType.REL16, o.imm & 0xFFFFFFFF)
                elif o.type == X86_OP_MEM:
                    op1 = _Op(decode16.OpType.MEM, o.mem.disp & 0xFFFFFFFF)
            by_addr[ins.address] = _Ins32(ins.address, ins.size, ins.mnemonic,
                                          ins.op_str, op1)
            added += 1
    else:
        pos = start
        while pos < min(len(code), start + limit):
            if pos in by_addr:
                break
            d = decode16.Decoder(code, 0)
            d.pos = pos
            try:
                ins = d.decode_one()
            except Exception:
                break
            if not ins or ins.length <= 0:
                break
            by_addr[ins.address] = ins
            added += 1
            pos += ins.length
    return added


def carve_functions(decode16, instructions, seg_len, roots, seed_unreached=False,
                    code=None, esize=2, valid=None):
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
            if r in claimed:
                continue
            if r not in by_addr and code is not None:
                # a real entry the sweep missed: realign on it
                resync(code, by_addr, r, esize, decode16)
            if r not in by_addr:
                continue
            body, calls = trace(by_addr, r, decode16, code=code,
                                esize=esize, boundaries=bounds, valid=valid)
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
    code, nfix = apply_selector_fixups(ne, a.seg, code)
    if nfix:
        print("applied %d selector fixups before lifting" % nfix)
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
    # The 32-bit segments announce their own entries. A 32-bit function
    # callable from this DLL's 16-bit half opens with a thunk that converts the
    # caller's frame and loads GS with the bitstream selector:
    #
    #     66 33 C0            xor ax, ax
    #     B8 CC CC CC CC      mov eax, <fixup placeholder>
    #     8C DB / 8E C3       mov ebx, ds / mov es, ebx
    #     8B D5 / 33 ED       mov edx, ebp / xor ebp, ebp
    #     66 8B EC            mov bp, sp
    #     66 8E 6D 08         mov gs, word ptr [ebp + 8]
    #
    # Match the BODY, not the placeholder. Matching `B8 CC CC CC CC` finds nine
    # entries and misses two more that are identical but for the placeholder
    # (0xF8F70000 at 0x2C10 and 0x3640) - and those two are the entries to the
    # last two unreached code runs in segment 3. Neither pattern is a superset
    # of the other, so take both.
    thunks = set()
    if is32:
        import re as _re
        for pat in (bytes((0x8C, 0xDB, 0x8E, 0xC3, 0x8B, 0xD5, 0x33, 0xED)),
                    bytes((0x8B, 0xD5, 0x33, 0xED, 0x66, 0x8B, 0xEC))):
            for m in _re.finditer(_re.escape(pat), code):
                q = m.start()
                if q >= 5 and code[q - 5] == 0xB8:      # back up over mov eax, imm32
                    q -= 5
                    if q >= 3 and code[q - 3:q] == bytes((0x66, 0x33, 0xC0)):
                        q -= 3                          # ...and the leading xor ax, ax
                thunks.add(q)
        pat = bytes((0x66, 0x33, 0xC0, 0xB8, 0xCC, 0xCC, 0xCC, 0xCC))
        thunks |= set(m.start() for m in _re.finditer(_re.escape(pat), code))
        extra |= thunks

    decoded_at = set(i.address for i in insns)
    prologues = set(o for o in range(len(code) - 2)
                    if code[o] == 0x55 and code[o + 1] == 0x8B and code[o + 2] == 0xEC
                    and o in decoded_at)
    extra |= prologues
    print("entries: %d far-call, %d exported, %d selector-reloc, %d prologue, "
          "%d thunk" % (len(far_entries), len(exported), len(reloc_entries),
                        len(prologues), len(thunks)))

    by_all = {i.address: i for i in insns}

    # Every indirect jump's table, harvested up front. They cannot be found by
    # descent alone: the tables sit inside functions nothing reaches yet, so
    # control flow never arrives to read them.
    valid = make_validator(code, is32, decode16)
    inline_jt, resolved = set(), 0
    for ins in insns:
        if (ins.mnemonic or '').lower() != 'jmp' or ins.op1 is None:
            continue
        if getattr(ins.op1, 'type', None) != decode16.OpType.MEM:
            continue
        t = table_after_jump(code, ins, esize, valid)
        if t:
            resolved += 1
            inline_jt.update(t)
    if inline_jt:
        print("jump tables: %d resolved -> %d targets" % (resolved, len(inline_jt)))
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
                                                 code=code, esize=esize, valid=valid)
        found = set()
        for addr in claimed:
            ins = by_addr.get(addr)
            if ins is None or (ins.mnemonic or '').lower() != 'jmp' or ins.op1 is None:
                continue
            if getattr(ins.op1, 'type', None) != decode16.OpType.MEM:
                continue
            found.update(table_after_jump(code, ins, esize, valid))
        # Also: a branch that leaves its own function needs the target to BE a
        # function. The lifter emits `goto` for a target inside the function it
        # is lifting and `dispatch(target)` for one outside, and dispatch can
        # only reach an entry - so a block two functions share, entered by a
        # branch from the one that does not contain it, has nowhere to land.
        # Eight of these showed up as unresolved dispatches the first time the
        # runtime ran the whole entry table. Splitting a function at such a
        # target costs nothing: it is a basic-block boundary either way.
        for _st, _body in fmap.items():
            _lo = _st
            _hi = max(by_addr[x].address + by_addr[x].length for x in _body)
            for _addr in _body:
                _ins = by_addr[_addr]
                _o = _ins.op1
                if _o is None or getattr(_o, 'type', None) != decode16.OpType.REL16:
                    continue
                _m = (_ins.mnemonic or '').lower()
                if not (_m == 'jmp' or _m == 'call' or _m in COND_JUMPS):
                    continue
                if _o.disp in by_addr and not (_lo <= _o.disp < _hi):
                    found.add(_o.disp)

        fresh = {t for t in found if t not in claimed and t in by_addr
                 and t not in tried}
        fresh |= {t for t in found if t in by_addr and t not in fmap
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
    # Report BYTES covered, not instructions reached. Resync adds instructions
    # the linear sweep never had, so "reached / swept" silently compares two
    # different denominators and drifts upward on its own. Bytes cannot.
    covered = bytearray(len(code))
    for _s, _body in funcs:
        for _i in _body:
            for _k in range(_i.address, min(len(code), _i.address + _i.length)):
                covered[_k] = 1
    nb = sum(covered)
    pct = 100.0 * nb / max(1, len(code))
    print("carved %d functions covering %d of %d bytes (%.1f%%)"
          % (len(funcs), nb, len(code), pct))
    if not a.seed_unreached and pct < 90.0:
        print("   %d bytes unreached - tables, or entry points we have not"
              % (len(code) - nb))
        print("   found yet (indirect calls). Not lifted; --seed-unreached forces it.")

    # A code segment returns. Zero returns across everything carved is not a
    # quirk of optimised code, it is the segment not being code - and the
    # failure is quiet, because a blob of data carves into exactly one enormous
    # "function" covering 100% of the segment, which reads as the best result
    # in the table. Segment 13 did precisely that: 1 function, 100% coverage,
    # and a lift whose only unhandled instructions were daa, das, aaa, bound,
    # arpl and packuswb - opcodes no video codec emits.
    #
    # This decides whether to lift at all, so it runs on every path and not
    # only under --stats, where it sat at first and did nothing for the driver
    # that actually generates the build.
    rets = sum(1 for _s, _b in funcs for _i in _b
               if (_i.mnemonic or '').lower() in ('ret', 'retf', 'retn',
                                                  'iret', 'iretd'))
    if rets == 0 and funcs:
        print("WARNING: not one return in %d carved functions - this segment"
              % len(funcs))
        print("         is almost certainly data, not code. Do not lift it.")

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

        # What is left over? "81% covered" means very different things
        # depending on whether the rest is code we cannot reach or data we must
        # never lift, so report the evidence instead of a verdict - earlier
        # attempts to classify this by entropy or by counting small dwords each
        # got a known case backwards.
        #
        # The one solid signal: a table of code pointers is mostly dwords that
        # are either zero or a valid address in this segment. Real code is not.
        runs, i = [], 0
        while i < len(code):
            if covered[i]:
                i += 1; continue
            j = i
            while j < len(code) and not covered[j]:
                j += 1
            runs.append((i, j - i)); i = j
        if runs:
            runs.sort(key=lambda r: -r[1])
            print("%d unreached runs, largest:" % len(runs))
            for off, n in runs[:6]:
                tot = ptr = zero = 0
                for k in range(off, off + n - (esize - 1), esize):
                    v = int.from_bytes(code[k:k + esize], "little")
                    tot += 1
                    if v == 0:
                        zero += 1
                    elif valid(v):
                        ptr += 1
                pct = 100.0 * (ptr + zero) / max(1, tot)
                ends = "ends at a known entry" if (off + n) in starts else ""
                print("   0x%04X  %5d bytes  %3.0f%% of %s are 0 or a valid "
                      "address (%d pointers)  %s"
                      % (off, n, pct, "dwords" if esize == 4 else "words",
                         ptr, ends))
        return 0

    # 32-bit segments go through pcrecomp's 32-bit lifter - the same one that
    # produced the Encarta executable - not lift16. It is written against a PE,
    # but only loosely: `read_va` is injectable and the image bounds are plain
    # attributes, so a 64K NE segment can pose as a tiny image based at 0.
    parts, unhandled = [], 0
    emitted_entries = []
    import re as _re
    _re_name = _re.compile(chr(92)+'bL_([0-9A-F]{8})'+chr(92)+'b')
    if is32:
        import lift32_cpu

        class _NELifter(lift32_cpu.Lifter):
            # Memory in a segmented build is not flat, and the base class has
            # two assumptions that only hold for a PE:
            #
            #   fs: is the thread block, so fs-relative access becomes
            #       __readfsdword. Here FS is one of the DLL's own data
            #       segments - IR32 keeps decoder state there - and the
            #       intrinsic would read the host thread's TIB instead.
            #   everything else is flat, so the segment is simply ignored.
            #       Here DS, ES and GS are three different objects: the DLL's
            #       data, the output frame, and the compressed bitstream, the
            #       last two handed in by the caller.
            #
            # So every access carries its segment, and the runtime turns a
            # selector into a base. Default is DS, or SS when the operand is
            # addressed through EBP or ESP, which is the x86 rule and matters
            # here because the 32-bit half reads its arguments off the 16-bit
            # caller's stack as `[ebp+6]`.
            def _seg_of(self, op):
                s = self.seg_name(op)
                if s:
                    return s
                m = op.mem
                base = self.md.reg_name(m.base) if m.base else None
                return "ss" if base in ("ebp", "esp", "bp", "sp") else "ds"

            def rd(self, insn, op):
                a = "SEGB(c->%s) + %s" % (self._seg_of(op), self.seg_off(insn, op))
                return {1: "rd8(%s)", 2: "rd16(%s)", 4: "rd32(%s)"}[op.size] % a

            def wr(self, insn, op, val):
                a = "SEGB(c->%s) + %s" % (self._seg_of(op), self.seg_off(insn, op))
                return {1: "wr8(%s, %s);", 2: "wr16(%s, %s);",
                        4: "wr32(%s, %s);"}[op.size] % (a, val)

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
            # lift32_cpu names functions L_<addr> from the segment-relative
            # address, so segment 2's L_00000000 and segment 3's are the same
            # symbol. Prefix per segment or they collide at link time.
            c = _re_name.sub(lambda m: "L_s%d_%s" % (a.seg, m.group(1)), c)
            unhandled += c.count("UNHANDLED") + c.count("abort()")
            emitted_entries.append(lo)
            parts.append(c)
    else:
        lifter = lift16.Lifter()
        # lift16 emits a direct call to `far_SSSS_OOOO` for a far transfer,
        # which suits a DOS binary where SSSS is a real paragraph. Here SSSS is
        # the target's NE segment number - the selector fixups above put it
        # there - so it is an address the runtime resolves, not a symbol. The
        # same rewrite covers imports, whose synthetic selector is 0xF0mm.
        far_call = _re.compile(r"\bfar_([0-9A-Fa-f]{4})_([0-9A-Fa-f]{4})\(cpu\)")
        near_call = _re.compile(r"\bres_([0-9A-Fa-f]{6})\b")
        for start, body in funcs:
            name = "ir32_s%d_%04X" % (a.seg, start)
            try:
                c = lifter.lift_function(name, body, start)
            except Exception as e:
                print("  !! %s failed: %s: %s" % (name, type(e).__name__, e))
                continue
            if isinstance(c, list):
                c = "\n".join(c)
            c = far_call.sub(
                lambda m: "recomp_dispatch(cpu, 0x%s, 0x%s)" % (m.group(1), m.group(2)), c)
            # lift16 names a near-call target `res_<offset>`, its own convention
            # from a flat DOS image. Ours are `ir32_s<seg>_<offset>`, so without
            # this every intra-segment call is an unresolved external - which
            # the compiler is perfectly happy with and the linker is not.
            c = near_call.sub(
                lambda m: "ir32_s%d_%04X" % (a.seg, int(m.group(1), 16)), c)
            unhandled += c.count("UNHANDLED") + c.count("abort()")
            emitted_entries.append(start)
            parts.append(c)

    out = a.out or ("ir32_seg%d.c" % a.seg)
    with open(out, "w") as f:
        f.write("/* AUTO-GENERATED by lift_ir32.py from IR32.DLL segment %d.\n"
                " * %s NE code lifted via pcrecomp %s.\n */\n"
                '#include <stdio.h>\n#include <stdlib.h>\n'
                '#include "%s"\n\n'
                % (a.seg,
                   "32-bit" if is32 else "16-bit",
                   "capstone + lift32_cpu" if is32 else "decode16 + lift16",
                   "recomp32.h" if is32 else "recomp16.h"))
        if is32:
            f.write(
                "/* Segmented 32-bit NE code. Every memory access carries its\n"
                " * segment: SEGB(sel) is the runtime's selector -> base map.\n"
                " * DS is the DLL's own data, ES the output frame, GS the\n"
                " * compressed bitstream - the last two supplied by the caller.\n"
                " * GVA() must be the identity: an NE jump table holds segment\n"
                " * offsets, not virtual addresses.\n"
                " */\n\n")

        # A near call can name a target in the part of the segment descent
        # never reached - segment 1 is 77% covered, and calls into the rest are
        # ordinary calls to code we simply do not have. Left alone they are
        # unresolved externals and nothing links at all, which stops the other
        # 34 segments being testable over a gap in one.
        #
        # So emit a stub that says which target is missing, at the moment it is
        # actually needed. A missing function that aborts by name is worth more
        # than a link error listing forty symbols with no context.
        body_text = "\n\n".join(parts)
        defined = set(emitted_entries)
        want = set()
        pat = (_re.compile(r"\bir32_s%d_([0-9A-F]{4})\(cpu\)" % a.seg) if not is32
               else _re.compile(r"\bL_s%d_([0-9A-F]{8})\(c\)" % a.seg))
        for m in pat.finditer(body_text):
            want.add(int(m.group(1), 16))
        missing = sorted(want - defined)
        if missing:
            print("   %d near-call targets are outside what descent reached; "
                  "stubbed" % len(missing))

        # Declarations first. A call to a function defined later in the file
        # gets an implicit `int f()` from C, which then conflicts with the real
        # `void f(CPU *)` - and functions within a segment call each other in
        # whatever order the code happens to sit.
        if emitted_entries:
            proto = ("void ir32_s%d_%04X(CPU *cpu);" if not is32
                     else "void L_s%d_%08X(CPU *c);")
            f.write("/* forward declarations: these call each other */\n")
            for off in sorted(set(emitted_entries) | set(missing)):
                f.write((proto % (a.seg, off)) + "\n")
            f.write("\n")
        f.write(body_text)
        f.write("\n")
        if missing:
            f.write("\n/* Targets of near calls that descent never reached.\n"
                    " * Not lifted, so calling one is a real gap - say which. */\n")
            for off in missing:
                if is32:
                    f.write("void L_s%d_%08X(CPU *c) { (void)c;\n"
                            "    fprintf(stderr, \"IR32: seg %d offset %04X was "
                            "never lifted\\n\"); abort(); }\n"
                            % (a.seg, off, a.seg, off))
                else:
                    f.write("void ir32_s%d_%04X(CPU *cpu) { (void)cpu;\n"
                            "    fprintf(stderr, \"IR32: seg %d offset %04X was "
                            "never lifted\\n\"); abort(); }\n"
                            % (a.seg, off, a.seg, off))
        if emitted_entries:
            ent = "ne16_entry" if not is32 else "ne_entry"
            name = ("ir32_s%d_%04X" if not is32 else "L_s%d_%08X")
            f.write("\n/* entry table: segment offset -> lifted function */\n")
            f.write("const %s ir32_seg%d_entries[] = {\n" % (ent, a.seg))
            for off in sorted(set(emitted_entries)):
                f.write("    { 0x%04Xu, %s },\n" % (off, name % (a.seg, off)))
            f.write("};\nconst unsigned ir32_seg%d_entry_count =\n"
                    "    sizeof(ir32_seg%d_entries)/sizeof(ir32_seg%d_entries[0]);\n"
                    % (a.seg, a.seg, a.seg))
    print("wrote %s: %d functions, %d lines, %d UNHANDLED instructions"
          % (out, len(parts), sum(p.count("\n") for p in parts), unhandled))
    return 0


if __name__ == "__main__":
    sys.exit(main())
