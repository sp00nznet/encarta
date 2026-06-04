"""Find a pure self-contained lifted->lifted chain in ENC97:
   caller A direct-calls leaf B; neither references imports; B has no calls.
   Prints candidates with arg-count guesses (from `ret N`) for differential testing.
"""
import struct, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CS_OP_MEM

EXE = r"C:\encarta\analysis\ENC97.EXE"
FUNCS = r"E:\ida\work\enc97\funcs.txt"

f = open(EXE, "rb").read()
pe = struct.unpack_from("<I", f, 0x3c)[0]
nsec = struct.unpack_from("<H", f, pe + 6)[0]
so = pe + 24 + struct.unpack_from("<H", f, pe + 20)[0]
base = struct.unpack_from("<I", f, pe + 24 + 28)[0]
secs = {}
text = None
for i in range(nsec):
    o = so + i * 40
    nm = f[o:o+8].split(b"\0")[0].decode()
    vs, va, rs, pr = struct.unpack_from("<IIII", f, o + 8)
    secs[nm] = (base+va, vs, pr, rs)
    if nm == ".text":
        text = (base+va, vs, pr, rs)

idata_lo = secs[".idata"][0]
idata_hi = idata_lo + secs[".idata"][1]
tlo, tvs, tpr, trs = text
thi = tlo + tvs

funcs = {}
order = []
for line in open(FUNCS):
    parts = line.split()
    if len(parts) >= 3 and parts[0].startswith("0x"):
        a = int(parts[0], 16); sz = int(parts[1]); funcs[a] = sz; order.append(a)

md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True

def code_for(addr, sz):
    off = tpr + (addr - tlo)
    return f[off:off+sz]

def analyze(addr):
    """return (has_call, calls_to[list], touches_import, ret_imm, has_indirect, only_arith)"""
    sz = funcs.get(addr, 0)
    if not sz or addr < tlo or addr+sz > thi:
        return None
    calls = []
    touches_import = False
    has_indirect = False
    ret_imm = 0
    insns = list(md.disasm(code_for(addr, sz), addr))
    for ins in insns:
        m = ins.mnemonic
        if m == "call":
            op = ins.operands[0]
            if op.type == 1:  # reg/imm? capstone: 1=reg,2=imm,3=mem
                pass
            if ins.operands[0].type == 2:  # imm direct
                calls.append(ins.operands[0].imm)
            else:
                has_indirect = True
        if m in ("jmp",) and ins.operands and ins.operands[0].type == 2:
            t = ins.operands[0].imm
            if not (addr <= t < addr+sz):
                calls.append(t)  # tail call
        if m.startswith("ret"):
            if ins.operands and ins.operands[0].type == 2:
                ret_imm = ins.operands[0].imm
        for op in ins.operands:
            if op.type == CS_OP_MEM:
                disp = op.mem.disp
                if idata_lo <= disp < idata_hi:
                    touches_import = True
    return (len(calls) > 0, calls, touches_import, ret_imm, has_indirect, insns)

# pure leaf: in funcs, no calls, no indirect, no import touch
pure_leaf = set()
for a in funcs:
    r = analyze(a)
    if r and not r[0] and not r[2] and not r[4]:
        pure_leaf.add(a)

print(f"pure leaves: {len(pure_leaf)} of {len(funcs)}", file=sys.stderr)

# find caller A: calls >=1 leaf, ALL its direct calls go to pure_leaf, no import touch, no indirect
cands = []
for a in funcs:
    r = analyze(a)
    if not r: continue
    has_call, calls, imp, ret_imm, indir, insns = r
    if not has_call or imp or indir: continue
    callees = [c for c in calls if c in funcs]
    if not callees: continue
    if all(c in pure_leaf for c in callees) and len(insns) < 60:
        cands.append((a, ret_imm, callees, len(insns)))

cands.sort(key=lambda x: x[3])
print(f"pure caller candidates: {len(cands)}")
for a, ret_imm, callees, ni in cands[:25]:
    print(f"  A=0x{a:x} ret={ret_imm} ninsn={ni} callees={[hex(c) for c in callees]} "
          f"(sizes {[funcs[c] for c in callees]})")
