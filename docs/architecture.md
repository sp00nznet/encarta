# Architecture

The parts, what each owns, and how data moves between them. Three languages:
Python generates, C runs, PowerShell and sh launch.

```
 your discs / install                     this repository                         output
 ─────────────────────                    ───────────────                         ──────
 ENC97.EXE ─┐                                                                     
 DECO_32.DLL├─► function map ─► tools/recomp/lift.py ─► lifted C (gitignored) ─┐
 ENCAPI32   ┘   (IDA/Ghidra)    (capstone, Python)                             │
                                                                               ▼
 IR32.DLL (16-bit) ─► tools/indeo/lift_all.py ─► lifted C ─► ir32vfw.dll   recomp_enc97_run.exe
                      (pcrecomp's 16-bit lifter)              (VfW codec)      (C harness, Win32)
                                                                   │               │
 CD1 content ──────────────────────────────────────────────────────┴──► run-encarta.ps1 / play.sh
 (.M20, .AVI, fonts)                                                     (Windows / Wine, CrossOver)

 .M20 / .STR / .DAT ─► m20dump, strdump, datdump, ftcdecode, mvbtext ─► files, PNGs, text
```

## The parts

**The lifter** (`tools/recomp/lift.py`, Python). Reads a PE and a function map
and writes one C function per x86 function, one statement per instruction,
against the CPU model in `cpu.h`. Calls and returns go through an emulated
stack, so argument layout matches x86 exactly. It owns no runtime state.
`find_*.py` read its output and pick out the functions the differential sweep
can compare. Everything it writes is derived from your copy and gitignored.

**The CPU model** (`tools/recomp/cpu.h`, `cpu_rt.c`, C). Guest registers are
`uint32_t` and hold real host addresses, which is why everything is a 32-bit
process. `dispatch` is declared here and defined by each harness.

**The harness** (`tools/recomp/recomp_enc97_run.c`, C). Maps `ENC97.EXE`,
applies its relocations, wires all 914 imports to the real DLLs, and runs the
lifted entry point. It owns the boundary in both directions: `call_machine`
runs real code on the emulated stack (and points the TIB at that stack so SEH
works there), and the real-to-lifted `__thiscall` trampoline lets MFC's virtual
calls land in lifted code. It also stands in for what Setup would have done:
`ENC97_PROFILE` answers registry and `.ini` lookups, `ENC97_REDIRECT` and
`ENC97_CDROM` move the content and make it look like a CD, and `ENC97_FONTS`
loads the CD's fonts for the process only.

**The video codec** (`tools/indeo/`, C and Python). The CD's 16-bit `IR32.DLL`
lifted with pcrecomp's 16-bit lifter, run on a small NE runtime, and wrapped
as a Video for Windows codec (`ir32vfw.dll`) that the harness installs with
`ICInstall`. Built by `runtime/build.bat`, not CMake, and needs a pcrecomp
checkout. Its own README is the long story.

**The launchers** (`tools/localcontent/run-encarta.ps1`, `play.sh`). Set the
harness's environment from one content folder. `play.sh` is the Linux and
macOS one: it maps the content as drive H: in the Wine prefix, copies MFC40 and
MSVCRT40 from CD1, and does `--headless --record` with Xvfb and ffmpeg.

**The data tools** (`tools/m20dump`, `strdump`, `datdump`, `ftcdecode`,
`indeo/indeodec`, `mvbtext`). Read Encarta's formats directly and run no
original code. Plain C (and Python for `mvbtext`), so they build natively
anywhere. `decooracle`, `fifdecode` and `spamdump` drive the original DLLs or
COM and are Windows-only.

**The conformance harness** (`tools/regress.py`). Runs the built programs
against the disc and reports pass, fail or skip per check, against the baseline
in `conformance.json`. A missing input is a skip, never a pass.

## Why the boundary is where it is

The recompiled code is only ENC97's own. MFC40, MSVCRT40 and EEUIL10 are real
DLLs, and so is Windows (or Wine) under them. That is why the harness, not the
lift, is where platform work lands: the three things that stopped Wine were
all in how the harness loads and calls real code
([porting.md](porting.md#what-it-took)). Moving that boundary outward, by
lifting EEUIL10 and then MFC40, is [ROADMAP](../ROADMAP.md) item 2, and the
route to a build that needs no Windows at all.
