"""funcmap.py - a function map for lift.py without IDA or Ghidra.

    py funcmap.py <ENC97.EXE> <funcs.txt>

Recursive descent from the entry point, the exports and every relocated
pointer into code (vtables, callbacks, message maps), following calls, jumps
and MSVC jump tables. Writes "0xADDR size sub_ADDR" lines, lift.py's format.
"""
import bisect
import sys
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import X86_OP_IMM, X86_OP_MEM

# ponytail: names of imports that never return, so tracing stops after them.
NORETURN = {b"ExitProcess", b"_exit", b"exit", b"abort", b"_CxxThrowException",
            b"RaiseException", b"ExitThread", b"FatalAppExitA", b"_amsg_exit"}


def build(path):
    pe = pefile.PE(path)
    base = pe.OPTIONAL_HEADER.ImageBase
    img = pe.get_memory_mapped_image()
    code = [(base + s.VirtualAddress, base + s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData))
            for s in pe.sections if s.Characteristics & 0x20000000]
    in_code = lambda a: any(lo <= a < hi for lo, hi in code)
    u32 = lambda va: int.from_bytes(img[va - base:va - base + 4], "little")
    padded = lambda t: 0 < t - base <= len(img) and img[t - base - 1] == 0xCC  # MSVC pads between functions with int3

    relocs = set()
    for br in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", []):
        for e in br.entries:
            if e.type == 3:
                relocs.add(base + e.rva)
    noret_iat = set()
    for d in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
        for imp in d.imports:
            if imp.name in NORETURN:
                noret_iat.add(imp.address)

    data = set()    # addresses code reads or writes as memory
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    cache = {}

    def insn(a):
        if a not in cache:
            cache[a] = next(md.disasm(img[a - base:a - base + 16], a), None)
        return cache[a]

    def thunk(t):
        """jmp [import]: the stubs the linker puts in one block for each DLL call."""
        i = insn(t)
        return i is not None and i.mnemonic == "jmp" and i.operands[0].type == X86_OP_MEM \
            and not i.operands[0].mem.base and not i.operands[0].mem.index

    def funclet(t):
        """MSVC's C++ EH code: an unwind funclet works in its parent's frame
        ([ebp-x] before any push ebp), a handler is mov eax, FuncInfo; jmp."""
        i = insn(t)
        if i is None or padded(t):
            return False
        if "ebp" in i.op_str and i.mnemonic != "push":
            return True
        j = insn(t + i.size)
        return i.mnemonic == "mov" and i.op_str.startswith("eax, 0x") and j is not None and j.mnemonic == "jmp"

    def trace(f, starts):
        """f's instructions {addr: size}, its calls out, and the jump tables it
        reads, or None if it runs into something that is not code."""
        seen, todo, calls, tables = {}, [f], set(), set()
        while todo:
            a = todo.pop()
            while a not in seen:
                i = insn(a)
                if i is None or not in_code(a):
                    return None
                seen[a] = i.size
                m, ops = i.mnemonic, i.operands
                data.update(o.mem.disp & 0xffffffff for o in ops if o.type == X86_OP_MEM)
                if m in ("ret", "retf", "iret", "int3", "hlt", "ud2"):
                    break
                if m == "call":
                    if ops[0].type == X86_OP_IMM:
                        calls.add(ops[0].imm & 0xffffffff)
                    elif ops[0].type == X86_OP_MEM and not ops[0].mem.base \
                            and ops[0].mem.disp & 0xffffffff in noret_iat:
                        break
                elif m.startswith("j"):
                    op = ops[0]
                    if op.type == X86_OP_IMM:
                        t = op.imm & 0xffffffff
                        if t != f and (t in starts or t < f or padded(t) or thunk(t)
                                       or img.find(b"\xcc\xcc", a - base, t - base) >= 0):
                            calls.add(t)      # a tail call: padding between is a function boundary
                        else:
                            todo.append(t)
                    elif op.type == X86_OP_MEM and m == "jmp" and op.mem.scale == 4 and not op.mem.base:
                        # jmp [reg*4 + table]: entries are relocated pointers into code
                        p = op.mem.disp & 0xffffffff
                        while p in relocs and in_code(u32(p)):
                            tables.add(p)
                            todo.append(u32(p))
                            p += 4
                    if m == "jmp":
                        break
                a += i.size
        return seen, calls, tables

    starts = {pe.OPTIONAL_HEADER.AddressOfEntryPoint + base}
    if hasattr(pe, "DIRECTORY_ENTRY_EXPORT"):
        starts |= {base + e.address for e in pe.DIRECTORY_ENTRY_EXPORT.symbols}
    funcs, covered, tables, called = {}, set(), set(), set()
    spans = []      # sorted (start, end) of everything traced

    def drain(todo):
        while todo:
            f = todo.pop()
            if f in funcs:
                continue
            r = funcs[f] = trace(f, starts)
            if r:
                covered.update(r[0])
                tables.update(r[2])
                called.update(r[1])
                bisect.insort(spans, (f, max(a + n for a, n in r[0].items())))
                new = {c for c in r[1] if in_code(c)} - starts
                starts.update(new)
                todo.extend(new)

    def inside(t):
        k = bisect.bisect_right(spans, (t, 1 << 32)) - 1
        return k >= 0 and spans[k][0] < t < spans[k][1]

    drain(sorted(starts))
    while True:
        # pointers into code not inside anything traced are functions too; in
        # address order, so a function is traced before the case labels and
        # tables a pointer finds inside it
        # (not one code reads as memory: a jump table or an index table)
        more = sorted({u32(p) for p in relocs - tables if in_code(u32(p))} - starts - covered - data)
        more = [t for t in more if not inside(t)]
        if not more:
            break
        for t in more:
            if t in covered or t in data or inside(t):
                continue
            starts.add(t)
            drain([t])

    out = {}
    for f, r in sorted(funcs.items()):
        if not r:
            continue
        end = max(a + n for a, n in r[0].items())
        g = max(out) if out else None
        if g is not None and f < g + out[g] and f not in called:
            continue    # a pointer into g's body: its jump or index table
        if funclet(f) and g is not None and f <= g + out[g] + 16:
            # an exception funclet right behind its function: IDA counts it as part of it
            out[g] = max(out[g], end - g)
        else:
            out[f] = end - f
    return out


if __name__ == "__main__":
    fm = build(sys.argv[1])
    with open(sys.argv[2], "w") as fh:
        for f in sorted(fm):
            fh.write(f"0x{f:08X} {fm[f]:6d} sub_{f:X}\n")
    print(f"{len(fm)} functions -> {sys.argv[2]}")
