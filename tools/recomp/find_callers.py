import struct, sys
path = sys.argv[1] if len(sys.argv) > 1 else r"C:\encarta\analysis\ENC97.EXE"
target = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x401D10
f = open(path, "rb").read()
pe = struct.unpack_from("<I", f, 0x3c)[0]
nsec = struct.unpack_from("<H", f, pe + 6)[0]
so = pe + 24 + struct.unpack_from("<H", f, pe + 20)[0]
base = struct.unpack_from("<I", f, pe + 24 + 28)[0]
for i in range(nsec):
    o = so + i * 40
    nm = f[o:o+8].split(b"\0")[0].decode()
    vs, va, rs, pr = struct.unpack_from("<IIII", f, o + 8)
    if nm == ".text":
        data = f[pr:pr+rs]
        callers = []
        for j in range(len(data) - 5):
            if data[j] == 0xE8:
                rel = struct.unpack_from("<i", data, j + 1)[0]
                site = base + va + j
                if site + 5 + rel == target:
                    callers.append(site)
        print("target", hex(target), "direct callers:", [hex(c) for c in callers], "count", len(callers))
