"""Inventory ENC97.EXE's import directory: per-DLL import counts, by-name vs
by-ordinal, and whether each DLL is resolvable on THIS system (LoadLibrary)."""
import struct, sys, ctypes
EXE = sys.argv[1] if len(sys.argv) > 1 else r"C:\encarta\analysis\ENC97.EXE"
f = open(EXE, "rb").read()
pe = struct.unpack_from("<I", f, 0x3c)[0]
nsec = struct.unpack_from("<H", f, pe + 6)[0]
opt = pe + 24
base = struct.unpack_from("<I", f, opt + 28)[0]
so = opt + struct.unpack_from("<H", f, pe + 20)[0]
secs = []
for i in range(nsec):
    o = so + i * 40
    nm = f[o:o+8].split(b"\0")[0].decode()
    vs, va, rs, pr = struct.unpack_from("<IIII", f, o + 8)
    secs.append((va, vs, pr, rs))
def rva2off(rva):
    for va, vs, pr, rs in secs:
        if va <= rva < va + max(vs, rs):
            return pr + (rva - va)
    return None
def cstr(off):
    e = f.index(b"\0", off)
    return f[off:e].decode("latin1")

imp_rva = struct.unpack_from("<I", f, opt + 96 + 1*8)[0]   # import dir
o = rva2off(imp_rva)
total = 0; byname = 0; byord = 0
dlls = []
k = ctypes.windll.kernel32
k.LoadLibraryA.restype = ctypes.c_void_p
k.GetProcAddress.restype = ctypes.c_void_p
k.GetProcAddress.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
while True:
    oft, tstamp, fchain, name_rva, first_thunk = struct.unpack_from("<IIIII", f, o)
    o += 20
    if name_rva == 0:
        break
    dname = cstr(rva2off(name_rva))
    thunk_rva = oft if oft else first_thunk
    to = rva2off(thunk_rva)
    n = 0; ords = 0; names = 0; sample = []
    h = k.LoadLibraryA(dname.encode())
    resolvable = bool(h)
    unresolved = 0
    while True:
        ent = struct.unpack_from("<I", f, to)[0]; to += 4
        if ent == 0:
            break
        n += 1; total += 1
        if ent & 0x80000000:
            ords += 1; byord += 1
            ordinal = ent & 0xFFFF
            if h:
                p = k.GetProcAddress(ctypes.c_void_p(h), ctypes.c_char_p(ordinal))
                if not p: unresolved += 1
            if len(sample) < 3: sample.append(f"#{ordinal}")
        else:
            names += 1; byname += 1
            nm = cstr(rva2off((ent & 0x7FFFFFFF) + 2))
            if h:
                p = k.GetProcAddress(ctypes.c_void_p(h), nm.encode())
                if not p: unresolved += 1
            if len(sample) < 3: sample.append(nm)
    dlls.append((dname, n, names, ords, resolvable, unresolved, sample))

print(f"ENC97 imports: {total} from {len(dlls)} DLLs ({byname} by-name, {byord} by-ordinal)\n")
print(f"{'DLL':<16}{'imports':>8}{'name':>6}{'ord':>6}  {'loads?':<7}{'unres':>6}  sample")
for d, n, names, ords, res, unres, sample in dlls:
    print(f"{d:<16}{n:>8}{names:>6}{ords:>6}  {('YES' if res else 'NO'):<7}{unres:>6}  {', '.join(sample)}")
