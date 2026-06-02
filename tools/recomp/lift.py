#!/usr/bin/env python3
"""
lift.py - mechanical x86 -> C static recompiler for DECO_32.DLL.

Disassembles a function (capstone) and emits a C function `void L_<addr>(CPU*)`
operating on the cpu.h runtime, one C statement per x86 instruction. Calls/returns
are modelled on the emulated stack exactly like x86 (caller pushes a return slot,
callee's `ret` pops it), so stack layout and arg access match the original.

Usage:
  py -3.11 lift.py <dll> <ida_funcs.txt> <out.c> 0xADDR [0xADDR ...]

ida_funcs.txt lines: "0xADDR  size  name" (from IDA). Used for function bounds.
"""
import sys
from capstone import *
from capstone.x86 import *

IMAGE_BASE = 0x11000000

# ---- register field mapping ----
R32 = {"eax","ecx","edx","ebx","esp","ebp","esi","edi"}
R16 = {"ax":"eax","cx":"ecx","dx":"edx","bx":"ebx","sp":"esp","bp":"ebp","si":"esi","di":"edi"}
R8L = {"al":"eax","cl":"ecx","dl":"edx","bl":"ebx"}
R8H = {"ah":"eax","ch":"ecx","dh":"edx","bh":"ebx"}

def reg_read(name):
    if name in R32: return f"c->{name}"
    if name in R16: return f"R16(c->{R16[name]})"
    if name in R8L: return f"R8L(c->{R8L[name]})"
    if name in R8H: return f"R8H(c->{R8H[name]})"
    raise NotImplementedError(f"reg_read {name}")

def reg_write(name, val):
    if name in R32: return f"c->{name} = (uint32_t)({val});"
    if name in R16: return f"SET16(c->{R16[name]}, {val});"
    if name in R8L: return f"SET8L(c->{R8L[name]}, {val});"
    if name in R8H: return f"SET8H(c->{R8H[name]}, {val});"
    raise NotImplementedError(f"reg_write {name}")

def reg_size(name):
    if name in R32: return 4
    if name in R16: return 2
    return 1

class Lifter:
    def __init__(self, dll_path, image_size):
        self.image_lo = IMAGE_BASE
        self.image_hi = IMAGE_BASE + image_size
        self.md = Cs(CS_ARCH_X86, CS_MODE_32)
        self.md.detail = True

    def in_image(self, a):
        return self.image_lo <= (a & 0xffffffff) < self.image_hi

    # ---- operand rendering ----
    def addr_expr(self, insn, op):
        m = op.mem
        terms = []
        base = self.md.reg_name(m.base) if m.base else None
        index = self.md.reg_name(m.index) if m.index else None
        d = m.disp & 0xffffffff
        if base: terms.append(f"c->{base}")
        if index: terms.append(f"c->{index}*{m.scale}")
        if base is None:
            terms.append(f"GVA(0x{d:08X})" if self.in_image(d) else f"0x{d:08X}u")
        elif d:
            terms.append(f"0x{d:08X}u")
        return "(" + " + ".join(terms) + ")"

    def rd(self, insn, op):
        sz = op.size
        a = self.addr_expr(insn, op)
        return {1:f"rd8({a})", 2:f"rd16({a})", 4:f"rd32({a})"}[sz]

    def wr(self, insn, op, val):
        sz = op.size
        a = self.addr_expr(insn, op)
        return {1:f"wr8({a}, {val});", 2:f"wr16({a}, {val});", 4:f"wr32({a}, {val});"}[sz]

    def src(self, insn, op):
        """read value of an operand"""
        if op.type == X86_OP_REG: return reg_read(self.md.reg_name(op.reg))
        if op.type == X86_OP_IMM:
            sz = op.size
            mask = {1:0xFF,2:0xFFFF,4:0xFFFFFFFF}[sz]
            return f"0x{op.imm & mask:X}u"
        if op.type == X86_OP_MEM: return self.rd(insn, op)
        raise NotImplementedError("src type")

    def dst_write(self, insn, op, val):
        if op.type == X86_OP_REG: return reg_write(self.md.reg_name(op.reg), val)
        if op.type == X86_OP_MEM: return self.wr(insn, op, val)
        raise NotImplementedError("dst type")

    def op_size(self, insn, op):
        return op.size

    # ---- per-instruction translation ----
    def translate(self, insn, labels):
        m = insn.mnemonic
        ops = insn.operands
        ea = insn.address
        nxt = insn.address + insn.size

        def two(): return ops[0], ops[1]
        def sz0(): return ops[0].size

        # arithmetic / logic
        if m == "mov":
            d, s = two(); return [self.dst_write(insn, d, self.src(insn, s))]
        if m == "lea":
            d, s = two(); return [reg_write(self.md.reg_name(d.reg), self.addr_expr(insn, s))]
        if m in ("add","sub","and","or","xor","adc","sbb"):
            d, s = two(); sz = sz0(); a = self._read_dst(insn, d); b = self.src(insn, s)
            if m == "add": r = f"flags_add(c, {a}, {b}, {sz})"
            elif m == "sub": r = f"flags_sub(c, {a}, {b}, {sz})"
            elif m == "adc": r = f"flags_adc(c, {a}, {b}, {sz})"
            elif m == "sbb": r = f"flags_sbb(c, {a}, {b}, {sz})"
            elif m == "and": r = f"flags_logicz(c, {a} & {b}, {sz})"
            elif m == "or":  r = f"flags_logicz(c, {a} | {b}, {sz})"
            elif m == "xor": r = f"flags_logicz(c, {a} ^ {b}, {sz})"
            return [self.dst_write(insn, d, r)]
        if m == "cmp":
            d, s = two(); return [f"flags_sub(c, {self._read_dst(insn,d)}, {self.src(insn,s)}, {d.size});"]
        if m == "test":
            d, s = two(); return [f"flags_logicz(c, {self._read_dst(insn,d)} & {self.src(insn,s)}, {d.size});"]
        if m == "inc":
            d = ops[0]; return [self.dst_write(insn, d, f"flags_incs(c, {self._read_dst(insn,d)}, {d.size})")]
        if m == "dec":
            d = ops[0]; return [self.dst_write(insn, d, f"flags_decs(c, {self._read_dst(insn,d)}, {d.size})")]
        if m == "neg":
            d = ops[0]; return [self.dst_write(insn, d, f"flags_sub(c, 0, {self._read_dst(insn,d)}, {d.size})")]
        if m == "not":
            d = ops[0]; return [self.dst_write(insn, d, f"(~({self._read_dst(insn,d)}))")]
        if m in ("shl","sal","shr","sar"):
            d = ops[0]; cnt = self.src(insn, ops[1]) if len(ops) > 1 else "1"
            fn = {"shl":"op_shl","sal":"op_shl","shr":"op_shr","sar":"op_sar"}[m]
            return [self.dst_write(insn, d, f"{fn}(c, {self._read_dst(insn,d)}, {cnt}, {d.size})")]
        if m == "movzx":
            d, s = two(); return [self.dst_write(insn, d, f"({self.src(insn,s)})")]
        if m == "movsx":
            d, s = two(); ssz = s.size
            cast = {1:"int8_t",2:"int16_t"}[ssz]
            raw = self._read_raw(insn, s)
            return [self.dst_write(insn, d, f"(uint32_t)(int32_t)({cast})({raw})")]
        if m == "cdq":
            return ["c->edx = (c->eax & 0x80000000u) ? 0xFFFFFFFFu : 0u;"]
        if m == "cwde":
            return ["c->eax = (uint32_t)(int32_t)(int16_t)R16(c->eax);"]
        if m == "xchg":
            d, s = two()
            return [f"{{ uint32_t _t = {self._read_dst(insn,d)}; " +
                    self.dst_write(insn, d, self.src(insn, s)).rstrip(';') + "; " +
                    self.dst_write(insn, s, "_t").rstrip(';') + "; }"]

        # stack
        if m == "push":
            return [f"push32(c, {self.src(insn, ops[0])});"]
        if m == "pop":
            d = ops[0]
            if d.type == X86_OP_REG: return [reg_write(self.md.reg_name(d.reg), "pop32(c)")]
            return [self.wr(insn, d, "pop32(c)")]

        # control flow
        if m == "jmp":
            t = ops[0]
            if t.type == X86_OP_IMM and t.imm in labels:
                return [f"goto L_{t.imm:08X};"]
            return [f"/* TODO tail/indirect jmp */ dispatch_jmp(c, {self._target(insn,t)}); return;"]
        if m == "call":
            t = ops[0]
            tgt = self._target(insn, t)
            return [f"push32(c, 0x{nxt:08X}u); dispatch(c, {tgt});"]
        if m == "ret":
            n = (ops[0].imm if ops and ops[0].type == X86_OP_IMM else 0)
            return [f"c->esp += {4 + n}; return;"]
        if m.startswith("j"):
            cond = self._cond(m)
            if cond is None: return [f"/* TODO {m} */ abort();"]
            t = ops[0]
            if t.type == X86_OP_IMM and t.imm in labels:
                return [f"if ({cond}) goto L_{t.imm:08X};"]
            return [f"/* TODO jcc out-of-func {m} */ abort();"]
        if m in ("nop","hint_nop"): return ["/* nop */"]
        if m == "leave":
            return ["c->esp = c->ebp; c->ebp = pop32(c);"]

        return [f"/* TODO {m} {insn.op_str} */ abort();"]

    def _read_dst(self, insn, op):
        # read a dst operand (for read-modify-write)
        if op.type == X86_OP_REG: return reg_read(self.md.reg_name(op.reg))
        if op.type == X86_OP_MEM: return self.rd(insn, op)
        raise NotImplementedError

    def _read_raw(self, insn, op):
        return self.src(insn, op)

    def _target(self, insn, op):
        if op.type == X86_OP_IMM: return f"0x{op.imm:08X}u"
        if op.type == X86_OP_REG: return reg_read(self.md.reg_name(op.reg))
        if op.type == X86_OP_MEM: return self.rd(insn, op)
        raise NotImplementedError

    def _cond(self, m):
        return {
            "je":"c->zf","jz":"c->zf","jne":"!c->zf","jnz":"!c->zf",
            "jbe":"(c->cf || c->zf)","jna":"(c->cf || c->zf)",
            "ja":"(!c->cf && !c->zf)","jnbe":"(!c->cf && !c->zf)",
            "jb":"c->cf","jc":"c->cf","jnae":"c->cf",
            "jae":"!c->cf","jnb":"!c->cf","jnc":"!c->cf",
            "jl":"(c->sf != c->of)","jnge":"(c->sf != c->of)",
            "jge":"(c->sf == c->of)","jnl":"(c->sf == c->of)",
            "jle":"(c->zf || (c->sf != c->of))","jng":"(c->zf || (c->sf != c->of))",
            "jg":"(!c->zf && (c->sf == c->of))","jnle":"(!c->zf && (c->sf == c->of))",
            "js":"c->sf","jns":"!c->sf","jo":"c->of","jno":"!c->of",
            "jp":"c->pf","jpe":"c->pf","jnp":"!c->pf","jpo":"!c->pf",
            "jecxz":"(c->ecx == 0)","jcxz":"(R16(c->ecx) == 0)",
        }.get(m)

    def lift_function(self, code, start):
        insns = list(self.md.disasm(code, start))
        # collect intra-function branch targets
        end = start + len(code)
        labels = set()
        for ins in insns:
            if ins.mnemonic.startswith("j") or ins.mnemonic in ("loop","loopne","loope"):
                for op in ins.operands:
                    if op.type == X86_OP_IMM and start <= op.imm < end:
                        labels.add(op.imm)
        out = []
        out.append(f"void L_{start:08X}(CPU *c)")
        out.append("{")
        for ins in insns:
            if ins.address in labels:
                out.append(f"L_{ins.address:08X}:")
            for line in self.translate(ins, labels):
                out.append(f"    {line:<60} /* {ins.address:08X}: {ins.mnemonic} {ins.op_str} */")
        out.append("}")
        return "\n".join(out)


def load_bounds(path):
    bounds = {}
    with open(path) as f:
        for line in f:
            parts = line.split()
            if len(parts) >= 3 and parts[0].startswith("0x"):
                bounds[int(parts[0], 16)] = int(parts[1])
    return bounds


def main():
    dll, funcs_txt, out_c = sys.argv[1], sys.argv[2], sys.argv[3]
    targets = [int(x, 16) for x in sys.argv[4:]]
    import pefile
    pe = pefile.PE(dll, fast_load=True)
    image_size = pe.OPTIONAL_HEADER.SizeOfImage
    # flat reader: VA -> bytes
    def read_va(va, n):
        rva = va - IMAGE_BASE
        return pe.get_data(rva, n)
    bounds = load_bounds(funcs_txt)
    lifter = Lifter(dll, image_size)

    chunks = ["/* AUTO-GENERATED by lift.py - do not edit */",
              '#include "cpu.h"', ""]
    for t in targets:
        size = bounds.get(t)
        if not size:
            print(f"[!] no size for {t:#x}", file=sys.stderr); continue
        code = read_va(t, size)
        chunks.append(lifter.lift_function(code, t))
        chunks.append("")
        print(f"[+] lifted L_{t:08X} ({size} bytes)", file=sys.stderr)
    with open(out_c, "w") as f:
        f.write("\n".join(chunks))
    print(f"[*] wrote {out_c}", file=sys.stderr)


if __name__ == "__main__":
    main()
