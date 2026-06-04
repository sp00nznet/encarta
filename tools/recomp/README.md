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

## ENC97.EXE (main app) — scoped, lifter proven to scale

The 1.32 MB MFC 4.0 application: **7,326 functions**, entry `0x50DB70`, **914
imports across 13 DLLs** (398 from MFC40 by ordinal + USER32/GDI32/KERNEL32/
MSVCRT40, the EEUIL10 UI library, and the DECO_32/ENCAPI32 DLLs already lifted).

**The entire binary lifts to compilable C.** All **7,326 functions** lift with
**zero unhandled opcodes**, and the complete output (`enc97_full.c`, ~452,000
lines) **compiles clean** under MSVC (a 6.3 MB object). lift.py gained the last
MSVC-4.x/MFC opcodes for full coverage: `setcc`, integer-operand x87
(`fidiv`/`ficom`…), transcendental x87 (`fsqrt`/`fsin`/`fcos`/`fpatan`/…),
`loop`/`enter`/`lodsw`/`int3`/`cld`, cross-function conditional tail-calls, and
mid-instruction jump targets routed through dispatch. The ~165 `abort()`s in the
output are jump-table default fall-throughs (correctly-lifted switches).

Regenerate (no addrs ⇒ lift every function in the funcs file):
```
py lift.py ENC97.EXE funcs.txt enc97_full.c
```

Lifted ENC97 code also **runs**, and is validated differentially against the
**real mapped originals**. `recomp_enc97.c` manual-maps ENC97.EXE at an
OS-chosen base (applying its 40,222 base relocations so absolute addresses — and
the lifted code's `GVA()`, via `g_image_delta` — resolve), then runs three
lifted code paths and compares each to the real function executed in place:

| # | Lifted path | Exercises | Check |
|---|-------------|-----------|-------|
| 1 | `sub_401D10` | Win32 import trampoline (`LoadCursorA`+`SetCursor`) | lifted==real==1 |
| 2 | `sub_4E3F40` → `sub_4FF190` | **lifted→lifted dispatch**; a real array search (loop + `lea`-based ×36 indexing) over a synthetic table | lifted==real for 5 present/absent/sentinel keys |
| 3 | `sub_4AD870` → `sub_4ADD10` | **lifted→lifted `__thiscall`** (`this` in `ecx`) + memory write | `*(this+0x8E)` lifted==real |

The dispatch table routes internal (original-VA) targets to the lifted C
function when present, else falls back to executing the real mapped original;
out-of-image targets are real Win32 APIs via the import trampoline. This proves
the lift executes correctly — across both the Win32 boundary and internal
lifted→lifted calls — not just that it compiles.

```
cmake --build build --config Release --target recomp_enc97
build\tools\recomp\Release\recomp_enc97.exe   # -> ALL PASS (3 differential checks)
```

### At full scale — the whole 7,326-function lift, exercised

`recomp_enc97_full` links the **complete** lift (`enc97_full.c`, all 7,326
functions, ~452k lines) and builds the **full 7,326-entry dispatch table**
(sorted, binary-searched). It then runs three checks:

- **[A]** table integrity — 7,326 entries, sorted, unique VAs, all non-null.
- **[B]** the three validated chains above, with their internal targets now
  resolved among all 7,326 functions through the real dispatch table.
- **[C]** a **differential sweep**: every *bounded straight-line* pure-leaf
  function (no calls/imports/writes/loops — so it cannot hang, unbounded-probe,
  or corrupt the stack) is run both lifted and as the real mapped original with
  identical register + stack state, and `eax` is compared. Result:

  ```
  PASS [C] no-write pure-leaf differential sweep: 818 matched, 0 mismatch, 11 skipped
  ```

  **818 distinct ENC97 functions match the real originals byte-exactly**; the 11
  skips faulted identically on the synthetic (zeroed) input. Because the lift was
  modelled at delta 0, address-returning leaves are compared modulo the load
  delta (same logical pointer).

This target is only built when the regenerated `enc97_full.c` is present locally
(it is too large to commit and is derived from the user's own `ENC97.EXE`).
Regenerate it and the sweep inputs with:
```
py lift.py ENC97.EXE funcs.txt enc97_full.c              # full 7,326-fn lift
py find_pure_chain.py                                    # -> enc97_pure_leaves.h
```

Notes from building it: MSVC's optimizer chokes for many minutes on the 40 MB
monolithic TU, so the target compiles `/Od` (codegen level doesn't affect lifted
semantics). The real originals are called on the real C stack — not via the
esp-switch trampoline — so a fault on synthetic input unwinds normally and is
caught (the esp-switch makes faults uncatchable).

The hard questions — "does the lifter handle a 1.3 MB MFC app?", "does lifted
app code execute against real Win32?", and "do lifted→lifted internal calls
dispatch and compute correctly?" — are all answered: **yes — the whole thing
lifts and compiles, and lifted functions run correctly through both the import
trampoline and lifted→lifted dispatch, matching the real originals.**

### Imports are fully satisfiable — the "MFC40 wall" was a myth

`recomp_enc97_iat` maps ENC97 and wires its **entire 914-import IAT**, reporting
what resolves on a modern system:

```
TOTAL: 914 imports across 13 DLLs (13 loaded) -> 914 resolved, 0 stubbed
100.0% of imports wired to real code
OS loader: LoadLibrary(ENC97.EXE) SUCCEEDED — every dependency (incl. MFC40
           from SysWOW64) is satisfiable on this Win11 system.
```

The earlier assumption that **MFC40.DLL** was an unobtainable blocker is wrong:
Windows 11 still ships `mfc40.dll`/`mfc40u.dll` in `SysWOW64`, and the 32-bit
`msvcrt.dll` there covers all 71 MSVCRT40 imports (including the x86 FP helpers
`_ftol`/`_CIpow`/`_adj_fdiv*` a 64-bit msvcrt lacks). The three Encarta-private
DLLs (`DECO_32`/`ENCAPI32`/`EEUIL10`) load from `analysis\` by full path —
pre-loading them satisfies ENC97's dependency on them, after which the real OS
loader maps and binds the whole EXE. So **all four prerequisites for a running
recomp are met**: the 7,326-entry dispatch table is built, every import wires to
real code, EEUIL10 needs no lift/stub (it loads), and only the MFC `CWinApp`
launch (entry → `InitInstance` → message loop, which wants the disc/data) remains
to actually boot the UI.

### The real app boots on Windows 11

As the end-to-end check of all the above, launching `ENC97.EXE` from `analysis\`
(so the three Encarta-private DLLs resolve) **boots**: it runs CRT + MFC init and
its own `InitInstance`, then reaches an Encarta dialog titled **"Fonts not
found"** (a resource string in `ENCRES97.DLL`). So the 1997 binary executes its
real startup on Win11 with no code/dependency blocker; getting past that dialog
is a content/install step — Encarta ships custom fonts its installer registers —
not a recompilation problem. (Launched under a watchdog that force-terminates the
modal dialog; nothing was installed system-wide.)

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
