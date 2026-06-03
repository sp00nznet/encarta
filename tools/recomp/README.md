# DECO_32 Static Recompilation

A **mechanical static recompilation** of the Encarta 97 FTC image codec
(`DECO_32.DLL`). The decode pipeline — header parsing, fractal decompression,
chroma reconstruction, and x87 color/scale math — is translated
instruction-by-instruction from the original x86 into C and recompiled to run
natively on modern Windows. Output is **byte-identical** to the original DLL on
all test images.

This is the recompilation track. The clean-room decoder lives in
`../ftcdecode`; the faithful DLL-bridge "oracle" used as ground truth lives in
`../decooracle`.

## Result

All 5 `picon_*.ftc` test images decode to pixels identical to the oracle
(CRC32 `74D4A38C 90702FFF 79AF78AF 3F11A6A5 5FEDBBE3`), with **no original code
executed** — the DLL is mapped only as a constant-data image.

## How it works

| File | Role |
|------|------|
| `cpu.h` | Runtime: CPU register/flag model (eager EFLAGS), partial-register semantics, direct 32-bit memory accessors, push/pop, and a full **x87 FPU** double-stack. |
| `lift.py` | The recompiler. Disassembles a function with capstone and emits `void L_<addr>(CPU*)` — one C statement per x86 instruction. Covers the integer ISA, x87, `mul/div/imul/idiv`, `rep` string ops, `pushfd/popfd`, and resolves `jmp [table]` switches into computed-goto chains. `call`/`ret` are modelled on the emulated stack so argument layout matches x86 exactly. |
| `lifted_codec.c` | Generated lift of the 28-function decode call graph. |
| `lifted_setup.c` | Generated lift of the 24-function setup graph (Open / SetFIFBuffer incl. the FTC/FIF parser / resolution / format). |
| `recomp_decode.c` | Integration: a `dispatch` table routes lifted→lifted calls and aborts (printing the address) on any unlifted target, so coverage is self-reporting; 5 C stubs implement the CRT boundary (`malloc`/`free`/`calloc`/`ftol`/heap-noop), which severs the heap subtree so **no KERNEL32 is reached** during decode; the single `GetVersion` indirect call is serviced directly. |
| `cpu_rt.c` | Shared runtime globals. |
| `test_vlc.c` | Differential fuzz harness: trampolines to the real DLL function and compares against the lifted version over millions of random inputs (used to validate the lifter on the VLC reader — 3,000,000 trials, 0 mismatches). |

The recompilation was driven leaf-up and validated at every step against the
oracle's golden pixel CRCs and per-function register traces
(`../../fif_test/oracle_out/`).

## Build & run

```powershell
cmake -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release --target recomp_decode

# decode + verify against the oracle CRC, optionally write a PNG
build\tools\recomp\Release\recomp_decode.exe fif_test\picon_000.ftc 74D4A38C out.png
```

By default the DLL is mapped as **data only** at its preferred base (no
`DllMain`, no original code). Set `RECOMP_NOMAP=1` to fall back to `LoadLibrary`.
You must supply your own legitimate `DECO_32.DLL` (path arg 4, or it is found
under `analysis/`); only its constant data tables are read — nothing is
redistributed here.

## Regenerating the lift

```powershell
py -3.11 lift.py <DECO_32.DLL> <ida_funcs.txt> lifted_codec.c 0xADDR 0xADDR ...
```

`ida_funcs.txt` (function `addr  size  name`) comes from IDA/Ghidra; it supplies
function boundaries. Capstone supplies the per-instruction disassembly.

## Second binary: ENCAPI32.DLL + Win32 import trampoline

`lift.py` now reads the PE's ImageBase, so it lifts any module. **ENCAPI32.DLL**
(base `0x10000000`, 36 functions, imports KERNEL32/USER32/ADVAPI32/MSVCRT40) is
lifted (`encapi_lifted.c`), and `recomp_encapi.c` runs it with a new
**import trampoline**: where the lifted code calls an imported API (`call
[IAT]`), `dispatch` detects the out-of-image target and invokes the **real
Win32 API** on the emulated stack (switch `esp` to the lifted arg area, call,
capture the API's resulting `esp` — which auto-handles stdcall cleanup). A
vectored handler continues the benign `DBG_PRINTEXCEPTION_C` from
`OutputDebugStringA`.

Validated: lifted `fGetArticleID` == the real export (string round-trip through
real `lstrcpyA`/`OutputDebugStringA`), plus the NULL-arg path. This proves the
lifted↔Win32 boundary — the prerequisite for recompiling the much larger
`ENC97.EXE` (which leans heavily on Win32/MFC).

```
cmake --build build --config Release --target recomp_encapi
build\tools\recomp\Release\recomp_encapi.exe   # -> PASS fGetArticleID
```

## Status / future

- [x] Decode path lifted, pixel-exact (FTC mode `01 01 02 01`, self-contained)
- [x] Setup path lifted (whole pipeline recompiled)
- [x] Data-only standalone mode (no original code executed)
- [x] FTC mode `04 03 04 01` (FTT-referenced) lifted — 110-function closure,
      decodes real PICON content to clean full colour
- [ ] Clean-room **regeneration** of the constant tables (lift the table
      builders) to remove the runtime dependency on the DLL data entirely
- [ ] Non-full-resolution scaling paths and the `"FIFF"` FIF variant
- [ ] Clean public C API + integration into a user-facing decoder

The recomp decodes both FTC encoding modes found in PICON.M20. For mode-04 it is
verified more robust than the DLL-bridge oracle (which exhibits a chroma-speckle
divergence under standalone setup); the recomp produces the correct clean image.
