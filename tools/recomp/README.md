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

### Standalone — no DLL file at runtime

`recomp_decode_standalone` runs the same lift with the DLL's data image
**reconstructed from a compiled-in blob**, so it needs **no `DECO_32.DLL` file**:

```powershell
py -3.11 gen_deco_data.py           # one-time: DECO_32.DLL -> deco32_data.c (gitignored)
cmake -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release --target recomp_decode_standalone
build\tools\recomp\Release\recomp_decode_standalone.exe fif_test\picon_000.ftc 74D4A38C
# -> PASS pixcrc=74D4A38C  (all 5 picons byte-exact, no DLL file touched)
```

`gen_deco_data.py` snapshots the DLL's section bytes (a build artifact, gitignored
like the lift output — not redistributed); at startup the standalone build
allocates the `0x11000000` image, lays the sections at their RVAs (`.bss`
zero-filled), and fixes the one `GetVersion` slot. `.text` is included even
though no original code executes — MSVC 4.x embeds the decoder's **jump tables
inline in `.text`**, which the lifted `switch` if-chains read via `GVA()`.

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

### The *lifted* entry point boots the app

`recomp_enc97_run` executes the **recompiled** entry point — `start@0x50DB70`,
the CRT/MFC bootstrap — as lifted C, through the 7,326-function dispatch table
with the full 914-import IAT wired to real Win32/MFC/CRT. It runs the complete
startup:

```
[1]  MSVCRT40!__set_app_type   [2] __p__fmode   [3] __p__commode  [7] _controlfp
[9]  MSVCRT40!_initterm        [10] __getmainargs  [12] _initterm  [13] __p__acmdln
[14] KERNEL32!GetStartupInfoA  [15] GetModuleHandleA
[18] MFC40#1368 (AfxWinMain)   -> MFC + ENC97 InitInstance
[19] MSVCRT40!exit(...)
*** lifted ENC97 reached exit after 1046 dispatched calls
    (182 C++/CRT init fns routed through LIFTED dispatch)
```

So the recompiled entry drives the entire CRT initialisation (set-app-type,
fp-control, the two `_initterm` C++/CRT init-table passes, arg parsing) and then
`AfxWinMain`, which runs MFC and ENC97's own `InitInstance`, before the app
terminates through the real `exit`. The harness also **intercepts `_initterm`**
and routes each entry of the C++/CRT init-pointer table back through the dispatch
table — so the **static initializers run as lifted code** (182 functions, ~1046
total lifted dispatches), exercising the *real→lifted* call path (the inverse of
the import trampoline) needed to run application code that real library code
invokes. Getting here required two new lifter capabilities, both essential for
any SEH-using / position-dependent code:

- **`fs:` segment access** (`mov eax, fs:[0]` → `__readfsdword(0)`): the CRT
  entry installs an SEH frame via the TIB; without this the lift null-derefs.
- **address-immediate relocation**: `push offset table` and similar absolute
  immediates are emitted as `GVA(imm)` when the `.reloc` table marks them, so the
  lifted code is position-independent (e.g. `_initterm`'s init-table bounds are
  now correct at any load base — previously they were stale VAs and faulted).

The CRT-level boot is fully lifted; `AfxWinMain`/`InitInstance` execute as the
real mapped code (reached via relocated vtable pointers — the hybrid handoff),
calling Win32/MFC through the wired IAT. In this uninstalled environment
`InitInstance` exits early (the same "Fonts not found" condition seen below).

#### Real→lifted trampoline + vtable routing (running the app body lifted)

To push the app *body* into lifted execution, the harness has the inverse of the
import trampoline — a **real→lifted `__thiscall` trampoline**. A per-target stub
(`mov eax, <ova>; jmp r2l_common`) lets *real* code (MFC virtual dispatch) call a
**lifted** function: it copies the caller's args onto a private emulated-stack
frame, seeds a `CPU` (`ecx`=`this`), runs the lifted function, and returns
`__thiscall`-correctly (popping the arg bytes the lifted `ret N` cleaned).
Reentrant (LIFO arena; result+pop returned in `edx:eax`). Validated in isolation
(`R2L_TEST`): a real `__thiscall` call into the trampoline runs lifted
`sub_4AD870→sub_4ADD10` and writes `this+0x8E` correctly.

`R2L_VTABLES` then rewrites every `.rdata`/`.data` slot that points at a
**function start** (so jump tables — whose entries are mid-function — are left
alone) to such a trampoline: **10,432 slots routed**, after which real MFC
virtual-dispatches land in lifted code. **The full-routing boot now runs clean**
— no heap corruption — all the way to the app's own UI. Both directions of the
lifted↔real boundary work: import trampoline (lifted→real) and this real→lifted
path, with `_initterm` running 182 init functions lifted.

Getting there took two harness fixes, both found by bisecting the routed slot
range (`R2L_LO`/`R2L_HI`) down to the single slot that broke the boot — slot 597,
`CWinApp::InitInstance` at `0x4050F0`:

- **`call_machine` never seeded `ebp`.** MSVC emits frameless funclets for SEH
  unwind and local-object destruction (`lea ecx,[ebp-0x10]; jmp ~CString`) that
  address the *caller's* frame. When such a funclet isn't in the lifted set the
  dispatcher falls back to the real original — which then ran against the host's
  `ebp` and destructed a garbage pointer. That was the heap corruption.
- **`call_machine` was not reentrant.** It kept the saved host `esp` in a global
  (`T_sesp`). The boot nests it: the outer call runs `AfxWinMain`, real MFC calls
  a routed vtable slot, the lifted callee dispatches an import — and the inner
  `call_machine` overwrote the outer's saved `esp`. `AfxWinMain` then returned
  onto a wrecked stack. The temporaries are now saved and restored per call.

The isolation switches that separated these from a lift bug are worth keeping:
`R2L_PASSTHRU` (rewritten slot jumps straight at the original), `R2L_STUB`
(trampoline returns 0 without running the callee), and `R2L_REAL=1|2` (run the
*original* code through the trampoline, with and without the esp switch). Those
three passing while the lifted **and** the real body both failed is what proved
the fault was in the trampoline, not in the recompiled code.

```
cmake --build build --config Release --target recomp_enc97_run   # needs enc97_full.c
build\tools\recomp\Release\recomp_enc97_run.exe analysis\ENC97.EXE 10000
```

#### The recompiled boot reaches Encarta's own UI

With the resource DLL resolvable (the harness adds the mapped app's directory to
the DLL search path, and resolves ENC97's private imports from there — that alone
took the IAT from 802/914 to **914/914**), the lifted boot runs CRT + MFC init,
`AfxWinMain` and `InitInstance`, and puts up **Encarta's own dialog**. Under full
vtable routing:

```
routed 10432 of 10432 vtable/fn-ptr slots -> real->lifted
ENC97 lifted boot: 1183 calls dispatched (182 init fns, 6 real->lifted vtable
calls), last lifted fn 0x50D1F4 - window: Encarta Encyclopedia cannot start
```

The dialog is the app's own, not a crash: without installed content and registry
state Encarta declines to start. Everything up to that point — including the
virtual dispatches MFC makes back into application code — executes lifted.

#### The app body runs recompiled

Two more things stood between the lifted boot and the application proper, and
neither was visible until the app got far enough to complain:

- **The command line.** The lifted CRT parses `_acmdln` - which is the *harness*
  process's command line - so ENC97 read `analysis/ENC97.EXE 20000` as its own
  parameters and refused to start ("The command line is improperly formatted").
  The harness now points `MSVCRT40!__p__acmdln` at a command line of the app's
  own (`ENC97_CMDLINE` overrides). `InitInstance` then returns TRUE and MFC calls
  the app's `Run()` - **the application body, executing lifted**.
- **A lifter bug the app body walked straight into.** `lift.py` wrapped
  relocatable address *immediates* in `GVA()` but not absolute *displacements
  inside memory operands* when the operand also had a base register:
  `mov dl, byte ptr [ecx + 0x56d902]` was emitted with the raw displacement, so
  at any load base but the preferred one it read from the unrelocated address.
  `.reloc` marks exactly these, so the same rule now drives both (280 such
  operands across the 7,326 functions).

With those fixed the lifted app runs its startup for real - palette setup,
`EEUIL10` UI-library init (`InitShruilDLL`, `CRefUIManager`), the Encarta startup
chime, device contexts, window classes - **2,200 dispatched calls, 250 of them
real MFC virtual dispatches landing in lifted code**, against 1,046 with routing
off. Running the same slots through `R2L_PASSTHRU` (real bodies) reaches exactly
the same point, so the recompiled body no longer diverges from the original.

Where it stops is Encarta's own install check, not the recompilation. It warns
that its custom fonts are missing (non-fatal, "will run without these fonts"),
then reports the generic "unknown error has occurred while initializing" from
`Run()` at `0x405380` and exits 0. Its registry probe (`REG_LOG`) shows only
absent user preferences - `SaveWindowLayout`, `TextStyle`, `JumpColor` - and its
HKLM writes denied for want of admin; none of that is fatal on its own. Getting
past it means giving the app the install state its Setup would have created.

One environment trap worth recording: Encarta calls `comdlg32!PrintDlgA` during
startup to size the default printer, and on this machine that stalls in the
spooler for ~44 s and takes the process with it. `NO_PRINTDLG` stubs it to FALSE;
everything above happens in 0.2 s with it set.

#### Encarta 97 starts

The app refused to start with "An unknown error has occurred while
initializing". Tracing it: `Run()` at `0x405380` maps an init status code to one
of nine messages, code 7 is the generic one, and `sub_4CBC70` returns 7 when the
`97Options` profile lookup for **`CodePath`** or **`DATPath`** comes back empty.
Those are what Setup records. They live under HKLM, which we can neither read
(Setup never ran) nor create (no admin), so the open failed and the value read as
absent.

Rather than install anything, the harness answers those lookups itself:
`ENC97_PROFILE="CodePath=...;DATPath=...;BookPath=..."` overrides the app's
profile reads (registry *and* `encarta.ini`), and while it is set an HKLM open
that fails falls back to HKCU. With CD1 mounted and `BookPath` pointed at it, the
app gets past its install check, loads content, and **opens its main window -
"Microsoft Encarta 97 Encyclopedia"**. Nothing is written to the machine.

```
recomp_enc97_run.exe analysis\ENC97.EXE 25000
  ENC97_PROFILE="CodePath=...\analysis\;DATPath=...\analysis\;BookPath=H:\ENCYC97\"
  NO_PRINTDLG=1 MSGBOX_LOG=1
```

#### Two more lifter bugs, found by bisecting the lifted set

Under full vtable routing the app instead ran 31,984 dispatched calls and died
on a virtual call through a null slot. `LIFT_LO`/`LIFT_HI` - new, and the
function-level twin of `R2L_LO`/`R2L_HI` - restrict which functions actually use
their lifted body, so binary-searching the range names the one whose lift is
wrong. It landed on `sub_4BB6F0`, the very function containing the faulting call:

```asm
004bb71f: push 4
004bb721: lea  edx, [esp + 0x18]
004bb728: push edx
004bb72b: mov  dword ptr [esp + 0x18], eax   ; stash the fn pointer
004bb72f: call dword ptr [esp + 0x18]        ; and call through it
```

The lifter emitted the return-address push *before* evaluating the call target:
`push32(c, ret); dispatch(c, rd32(c->esp + 0x18))`. Real `call` reads its target
with the pre-push `esp`, so the lifted version was reading one slot off - a zero.
Indirect calls now resolve the target into a temporary first.

The same hunt turned up `call_machine` capturing only `eax`: a callee returning a
64-bit value in `edx:eax` silently lost the high half, and this code passes such
pairs around constantly (`mov [ebp-8],eax; mov [ebp-4],edx`).

One trap worth recording about the bisect tool itself: `rewrite_fnptr_slots`
decided "is this a function start?" with the same `lookup()` that `LIFT_LO/HI`
filters, so restricting the lifted set silently changed which vtable slots got
routed - and the first bisect blamed a thunk that never executes. It uses an
unfiltered lookup now.

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
- [x] **No DLL file at runtime** — `recomp_decode_standalone` reconstructs the
      data image from a generated blob; 5/5 picons byte-exact with no `DECO_32.DLL`
- [ ] Non-full-resolution scaling paths and the `"FIFF"` FIF variant
- [ ] Clean public C API + integration into a user-facing decoder

The recomp decodes both FTC encoding modes found in PICON.M20. For mode-04 it is
verified more robust than the DLL-bridge oracle (which exhibits a chroma-speckle
divergence under standalone setup); the recomp produces the correct clean image.
