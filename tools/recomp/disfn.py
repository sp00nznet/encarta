import struct, sys, os
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
EXE = os.path.join(os.path.dirname(__file__), "..", "..", "analysis", "ENC97.EXE")
FUNCS = r"E:\ida\work\enc97\funcs.txt"
f = open(EXE, "rb").read()
pe = struct.unpack_from("<I", f, 0x3c)[0]
nsec = struct.unpack_from("<H", f, pe + 6)[0]
so = pe + 24 + struct.unpack_from("<H", f, pe + 20)[0]
base = struct.unpack_from("<I", f, pe + 24 + 28)[0]
for i in range(nsec):
    o = so + i * 40
    nm = f[o:o+8].split(b"\0")[0].decode()
    vs, va, rs, pr = struct.unpack_from("<IIII", f, o + 8)
    if nm == ".text":
        tlo, tpr = base+va, pr
funcs = {}
for line in open(FUNCS):
    p = line.split()
    if len(p) >= 3 and p[0].startswith("0x"):
        funcs[int(p[0],16)] = int(p[1])
md = Cs(CS_ARCH_X86, CS_MODE_32)
for a in sys.argv[1:]:
    a = int(a, 16); sz = funcs.get(a, 32)
    off = tpr + (a - tlo)
    print(f"=== 0x{a:x} size={sz} ===")
    for ins in md.disasm(f[off:off+sz], a):
        print(f"  {ins.address:08x}: {ins.mnemonic:8s} {ins.op_str}")
