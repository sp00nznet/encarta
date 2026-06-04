"""Probe whether the local/redirect DLLs actually load + cover their imports."""
import ctypes, struct
k = ctypes.windll.kernel32
k.LoadLibraryA.restype = ctypes.c_void_p
k.GetProcAddress.restype = ctypes.c_void_p
k.GetProcAddress.argtypes = [ctypes.c_void_p, ctypes.c_char_p]

for path in [r"C:\encarta\analysis\EEUIL10.DLL", r"C:\encarta\analysis\ENCAPI32.DLL",
             r"C:\encarta\analysis\DECO_32.DLL", r"msvcrt.dll", r"MFC40.DLL", r"mfc42.dll"]:
    h = k.LoadLibraryA(path.encode())
    print(f"{path:<38} {'LOADED @%08X' % h if h else 'FAILED gle=%d' % k.GetLastError()}")

# Does modern msvcrt cover MSVCRT40's by-name imports?
EXE = r"C:\encarta\analysis\ENC97.EXE"
f = open(EXE, "rb").read()
pe = struct.unpack_from("<I", f, 0x3c)[0]
nsec = struct.unpack_from("<H", f, pe + 6)[0]
opt = pe + 24
so = opt + struct.unpack_from("<H", f, pe + 20)[0]
secs = []
for i in range(nsec):
    o = so + i*40; vs, va, rs, pr = struct.unpack_from("<IIII", f, o+8)
    secs.append((va, vs, pr, rs))
def r2o(rva):
    for va, vs, pr, rs in secs:
        if va <= rva < va + max(vs, rs): return pr + (rva - va)
def cstr(off):
    return f[off:f.index(b'\0', off)].decode('latin1')
imp = struct.unpack_from("<I", f, opt+96+8)[0]
o = r2o(imp)
mh = k.LoadLibraryA(b"msvcrt.dll")
while True:
    oft, ts, fc, nr, ft = struct.unpack_from("<IIIII", f, o); o += 20
    if nr == 0: break
    if cstr(r2o(nr)).lower() != "msvcrt40.dll": continue
    to = r2o(oft if oft else ft); miss = []; tot = 0
    while True:
        e = struct.unpack_from("<I", f, to)[0]; to += 4
        if e == 0: break
        tot += 1
        if not (e & 0x80000000):
            nm = cstr(r2o((e & 0x7FFFFFFF) + 2))
            if not k.GetProcAddress(ctypes.c_void_p(mh), nm.encode()): miss.append(nm)
    print(f"\nMSVCRT40 via msvcrt.dll: {tot-len(miss)}/{tot} resolved; missing: {miss}")
