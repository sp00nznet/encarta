# Getting started

Six stages, each one useful on its own. Stop at whichever gets you what you
came for — most people want stage 1 or 3 and never need 6.

Every step below was run from a clean `git clone` of this repository on
Windows 11 while writing this page. The Linux and macOS route is noted where it
differs, and in full in [porting.md](porting.md). Where something cannot be reproduced from a
clone, it says so and why, rather than leaving you to find out.

| Stage | What you get | What it costs you |
|---|---|---|
| [1](#1-build-the-tools) | 16 command-line tools | VS 2022, CMake, Python |
| [2](#2-get-the-content) | the disc mounted or mirrored | a legitimate copy of Encarta 97 |
| [3](#3-read-the-encyclopedia-text) | 31,108 titles, article prose | stage 1 + 2 |
| [4](#4-decode-the-pictures) | 11,350 images to PNG | stage 3 + your `DECO_32.DLL` |
| [5](#5-the-video-codec) | byte-exact Indeo 3 decode, and video in the app | a [pcrecomp][pcr] checkout |
| [6](#6-run-the-application) | Encarta 97 running as recompiled C | stage 1 + 2, about 10 minutes |

[pcr]: https://github.com/sp00nznet/pcrecomp

## Prerequisites

- **Windows 10 or 11, x64**, with **Visual Studio 2022** and the *Desktop
  development with C++* workload. Everything targets **Win32/x86**, not x64;
  that is not a preference, it is [structural](#why-32-bit-windows-code).
- **Or Linux or macOS**, with clang, lld, CMake, Ninja and
  [xwin](https://github.com/Jake-Shadle/xwin) to cross-build that same Win32
  code, and Wine (Linux) or CrossOver (macOS) to run it. The data tools also
  build natively with gcc or clang.
- **CMake 3.20 or newer** (`cmake_minimum_required(VERSION 3.20)`).
- **Python 3.11+** with `pefile`, `capstone` and `numpy`:
  ```powershell
  py -m pip install pefile capstone numpy
  ```
  `numpy` is only used by the Indeo verifiers (stage 5), the other two by the
  lifter.
- **ffmpeg on `PATH`** — only for stage 5, which checks our decoder against it.
- **A legitimate copy of Microsoft Encarta 97 Encyclopedia.** Nothing from the
  product is distributed here. Which edition matters:

  | Edition | Works | Note |
  |---|---|---|
  | Encarta 97 Encyclopedia, English, **2-CD Deluxe** | yes | what everything here was developed against |
  | Encarta 97 Encyclopedia, English, 1-CD | mostly | same engine; CD2-only media is absent |
  | Encarta 97 **World Atlas**, any Encarta 96/98/99 | untested | see [Other editions](../ROADMAP.md#other-encarta-editions) |

## 1. Build the tools

```powershell
git clone https://github.com/sp00nznet/encarta
cd encarta
cmake -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release
```

That is the whole of stage 1 and it needs no disc. It produces 11 executables
under `build\tools\*\Release\`:

```
datdump  decooracle  decotrace  fifdecode  ftcdecode  ftcprobe  indeodec
m20dump  recomp_enc97_iat  spamdump  strdump
```

On Linux or macOS, `./build.sh` makes the same 11 as Win32 executables under
`build-x86/` (run `xwin --accept-license --arch x86 splat --output ~/.xwin`
once first), and `cmake -S . -B build && cmake --build build` makes the six data
tools natively.

The rest appear only once you have generated their inputs from your own copy of
the product, because those inputs are derived files we do not redistribute:

| Target | Needs | Made in |
|---|---|---|
| `recomp_decode` | `tools/recomp/lifted_codec.c` | [stage 4](#4-decode-the-pictures) |
| `recomp_decode_standalone` | that, and `tools/recomp/deco32_data.c` | [stage 4](#4-decode-the-pictures) |
| `recomp_enc97_run` | `tools/recomp/enc97_full.c` | [stage 6](#6-run-the-application) |
| `recomp_enc97_full` | that, and the `find_*.py` headers | [tools/recomp](../tools/recomp/README.md) |
| `recomp_encapi`, `recomp_enc97`, `test_vlc`, `test_vlc_auto` | their own small lifts | [tools/recomp](../tools/recomp/README.md) |

CMake guards both with `if(EXISTS ...)`, so a clone without them configures and
builds cleanly rather than failing — which is also why they are silently absent
if you skip ahead.

**If a rebuild suddenly errors with `Parse error in cache file`,** something
outside this project has overwritten `build\CMakeCache.txt`. Delete the `build`
directory and re-run the two commands above; nothing in it is precious.

## 2. Get the content

Two ways. Mounting is quicker; mirroring is what you want if you plan to run the
application, because then the disc can go back in its case.

**Mount an image** (PowerShell, as yourself):

```powershell
Mount-DiskImage -ImagePath (Resolve-Path .\CD1ENC97ENC.iso)
```

**Or mirror it once** to a hard disk — ~645 MB, and after this no disc is ever
read again:

```powershell
cd tools\localcontent
.\mirror-cd.ps1 -Source H:\ -Dest G:\encarta97
```

Either way you end up with a directory containing `ENCYC97\` and `AAMSSTP\`.
Everything below calls that `$CD1`. Set it once and copy the app out:

```powershell
$CD1 = "G:\encarta97"                  # or "H:\" if you mounted the image

# the app and its DLLs, and the two Setup would have put in System32
mkdir analysis
copy $CD1\AAMSSTP\ENCARTA\*           analysis\
copy $CD1\AAMSSTP\SYSTEM32\AM16.DLL   analysis\
copy $CD1\AAMSSTP\SYSTEM32\AMF16.DLL  analysis\
```

`analysis\` is gitignored. `AM16.DLL` and `AMF16.DLL` are PE32 despite the
names; without them `ENCTITLE.DLL` cannot load and article pictures fail with
*"Encarta is not set up properly"*.

## 3. Read the encyclopedia text

```powershell
build\tools\m20dump\Release\m20dump.exe -x $CD1\ENCYC97\ENCARTA.M20 -o analysis\enc_dir
py tools\mvbtext\mvbtext.py analysis\enc_dir titles          # 31,108 titles
py tools\mvbtext\mvbtext.py analysis\enc_dir grep Einstein
py tools\mvbtext\mvbtext.py analysis\enc_dir prose _00006060 # the Russia article
```

`ENCARTA.M20` extracts to 32,535 internal files, so give it its own directory.

If you put `enc_dir` anywhere other than `analysis\enc_dir` or `enc_dir`, tell
the regression suite where it is with **an absolute path**:

```powershell
$env:ENC97_M20 = "D:\wherever\enc_dir"     # absolute; a relative path
                                           # silently fails three checks
```

## 4. Decode the pictures

First lift the codec from your own `DECO_32.DLL`. That needs a function map,
made the same way as ENC97's in [stage 6](#6-run-the-application). `--exports`
lifts what the DLL's 28 exports reach by direct call, so no list of addresses
has to be kept anywhere:

```powershell
py tools\recomp\lift.py analysis\DECO_32.DLL deco_funcs.txt tools\recomp\lifted_codec.c --exports
cmake -B build -G "Visual Studio 17 2022" -A Win32          # picks up the new file
```

Then:

```powershell
build\tools\m20dump\Release\m20dump.exe -x $CD1\ENCYC97\PICON.M20 -o picon_dir
build\tools\recomp\Release\recomp_decode.exe picon_dir\T000009D.FSM "" out.png
```

Use `-x` and **not** `-d`: the FTC entries are not LZ77-compressed and `-d`
corrupts them. `PICON.M20` holds 11,350 `.FSM` images and 49 shared `.FTT`
fractal transform tables, which are loaded automatically from the same
directory.

`recomp_decode` maps `DECO_32.DLL` as **constant data only** — no original code
runs — so it needs the file. It looks in `analysis\DECO_32.DLL` relative to the
current directory; override with the `DECO32_DLL` environment variable or a
fourth argument.

To drop that last dependency, generate the data blob from your own copy and
build the standalone target, which needs no `DECO_32.DLL` at all:

```powershell
py tools\recomp\gen_deco_data.py analysis\DECO_32.DLL tools\recomp\deco32_data.c
cmake -B build -G "Visual Studio 17 2022" -A Win32          # picks up the new file
cmake --build build --config Release --target recomp_decode_standalone
```

## 5. The video codec

All 68 clips are Indeo 3.2, and the only Indeo driver on the disc is 16-bit, so
it was statically recompiled. Building that lift needs the 16-bit lifter, which
lives in **[pcrecomp][pcr]**, not here:

```powershell
git clone https://github.com/sp00nznet/pcrecomp
$env:PCRECOMP = "C:\src\pcrecomp"          # the checkout root
cd tools\indeo\runtime
.\build.bat $CD1\AAMSSTP\SYSTEM16\IR32.DLL
```

**Use pcrecomp at `c5376ff` for this,** not today's `main`. Its commit
`deccd6a` (2026-09-13) walks a jump table until a dword is not an address in
the image, which an NE segment's small offsets never stop, and the codec it
lifts decodes only 31 of 68 clips exactly; `c5376ff`, just before it, decodes
all 68. `Setup.cmd` notices and fetches `c5376ff` for this step by itself.

`build.bat` finds `vcvars32.bat` itself, lifts forty code segments, and produces
`build\ir32_run.exe`, `build\ir32_vfwtest.exe` and `build\ir32vfw.dll`. It stops
with a message if `PCRECOMP` does not point at a checkout.

Check it against ffmpeg — every frame, every clip:

```powershell
cd tools\indeo
py verify_frames.py            # first frames, byte-exact
py verify_rgb.py               # what ICM_DECOMPRESS hands back
py verify_vfw.py               # the same through Video for Windows
```

Each resolves `IR32.DLL`, the AVI directory and ffmpeg via `tools/indeo/paths.py`
— override with `ENC97_IR32`, `ENC97_AVI`, `ENC97_FFDIR`. If any is missing they
say so and stop; they will not report "0 of 0" as if that were a result.

## 6. Run the application

`Setup.cmd` / `setup.sh` do all of this stage in one go. By hand:

Lifting `ENC97.EXE` needs a **function map**: where each function starts and
how long it is. It is derived from your own copy, so like the lift it is not
distributed here. `funcmap.py` makes it, with no disassembler to install:

```powershell
py tools\recomp\funcmap.py analysis\ENC97.EXE tools\recomp\funcs.txt
```

It traces from the entry point and every relocated pointer into code, following
calls, jumps and jump tables, in about 20 seconds. Measured against IDA's map on
2026-10-21: 7,264 functions to IDA's 7,326, **7,035 identical** in start and
size. The 220 that IDA has and it lacks are referenced nowhere in the binary
(no call, no pointer), so nothing can reach them; the rest differ in where a
function's tail is counted. A lift from it builds, reaches the main window, and
passes `regress.py`.

A disassembler's map works too. `lift.py` wants one function per line,
`0xADDR  size  name`, and neither disassembler emits exactly that, so both
paths end in a one-line conversion.

**With Ghidra** (free). Run pcrecomp's `DumpBounds.java` headless — note
`-scriptPath`, or Ghidra will not find the script — and turn its `start,end`
(end exclusive) CSV into sizes:

```powershell
analyzeHeadless C:\ghidra-proj enc97 -import analysis\ENC97.EXE `
    -scriptPath C:\src\pcrecomp\tools\ghidra `
    -postScript DumpBounds.java C:\work\enc97\bounds.csv

py -c "import sys; [print('0x%08X %6d sub_%X' % (int(a,16), int(b,16)-int(a,16), int(a,16))) for a,b in (l.strip().split(',') for l in open(sys.argv[1]) if ',' in l)]" C:\work\enc97\bounds.csv > tools\recomp\funcs.txt
```

**With IDA Pro**, pcrecomp's `tools/ida/ida_funcs.py` writes JSON with an `ea`
and `size` per function, so the conversion is the same shape:

```powershell
py -3.11 C:\src\pcrecomp\tools\ida\ida_funcs.py analysis\ENC97.EXE C:\work\enc97\funcs.json
py -c "import json,sys; [print('0x%08X %6d %s' % (f['ea'], f['size'], f['name'])) for f in json.load(open(sys.argv[1]))['functions']]" C:\work\enc97\funcs.json > tools\recomp\funcs.txt
```

IDA is where the **7,326** functions quoted throughout these docs came from.

Writing `funcs.txt` to `tools\recomp\` is deliberate: the analysis scripts
(`find_pure_chain.py` and friends) look for it there, or at `%ENC97_FUNCS%`.

Then lift the whole binary — no target addresses means every function in the
map — and build the harness:

```powershell
py tools\recomp\lift.py analysis\ENC97.EXE tools\recomp\funcs.txt `
        tools\recomp\enc97_full.c
cmake -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release --target recomp_enc97_run
```

`enc97_full.c` comes out at about 40 MB and 452,000 lines. The `recomp_enc97_run`
target compiles it at `/Od` on purpose — the optimizer spends many minutes on a
translation unit that size and codegen level cannot change lifted semantics.
Expect a long build once.

Run it against a mirror:

```powershell
cd tools\localcontent
.\run-encarta.ps1 -Content G:\encarta97 -Hold
```

`-Hold` keeps the window open so you can click around. The script sets the
environment variables the harness needs (`ENC97_PROFILE`, `ENC97_CDROM`,
`ENC97_REDIRECT`, `ENC97_FONTS`, `IR32_DLL`) and prints how to undo the `subst`
it makes. `ENC97_PROFILE` answers what Setup would have written to the registry,
`Deluxe`, `DiscPath0` and the paths among them, so nothing is installed.

On Linux or macOS, after `./build.sh`:

```sh
./play.sh /path/to/encarta97-mirror --hold                        # Wine / CrossOver
./play.sh /path/to/encarta97-mirror --headless --record run.mp4   # Linux, no window
```

`play.sh` does the same job, maps the content as drive H: in the Wine prefix,
and copies MFC40 and MSVCRT40 from CD1 beside `ENC97.EXE`, since Wine has
neither. There is no video yet; [porting.md](porting.md) says why.
Copy `tools\indeo\runtime\build\ir32vfw.dll` next to `recomp_enc97_run.exe`
first, or video will not play — the CMake build does that for you if stage 5 ran
before this one.

## Check it all still works

```powershell
py tools\regress.py            # 10 checks, ~40s
py tools\regress.py --full     # every frame rather than six, ~70s
py tools\regress.py --list     # what it would run, and why each might skip
```

A check whose inputs are missing reports **SKIP**, never a pass, so this is
usable with no disc in the machine. On a complete setup all ten pass:

```
  function map         ... ok    7264 functions, the IDA-checked ones exact
  codec loads          ... ok    init matches the disassembly
  decoder byte-exact   ... ok    68 of 68 frames byte-exact
  ICM returns picture  ... ok    68 of 68 frames correct through ICM
  VFW finds codec      ... ok    68 of 68 frames through Video for Windows
  phrase encoding      ... ok    the four anchor codes and the single-byte form
  article text         ... ok    2 articles read as prose
  media list           ... ok    25 media references, captions intact
  app reaches window   ... ok    main window, 1045 dispatched calls
  leaves match real    ... ok    974 functions match the originals
```

The three video checks are the memory-hungry ones: they spawn a process per
clip, each reserving a 512 MB stack, and compare frames through numpy. On a
machine already short of commit charge they can fail with
`WinError 1455 (paging file too small)` or `OpenBLAS error: Memory allocation
still failed`. Both are resource pressure, not decoder faults — the same checks
pass on a re-run. If you see either while something large is compiling, that is
why.

## Why 32-bit Windows code

Everything here is 32-bit x86 Windows code, even when it runs on Linux or macOS
(under Wine), and both halves of that are load-bearing.

**32-bit** is the lifter's memory model. `cpu.h` keeps the guest registers as
`uint32_t` and a lifted `mov eax, [ebx]` dereferences `ebx` as a real host
pointer — so every address the recompiled code touches has to fit in 32 bits.
That is why targets are linked `/LARGEADDRESSAWARE:NO`. ENC97 itself no longer
has to land at its preferred base: the harness applies its relocations and the
lifter wraps every relocatable immediate in `GVA()`, so it runs wherever the OS
puts it (delta +35,454,976 on Windows 11 and +8,323,072 under Wine, both
measured 2026-10-14). `recomp_enc97_run` is still based at `0x60000000`, which
keeps the host out of the low range. Moving to 64-bit is not a compiler flag,
it is a different memory model.

**Windows** is what is underneath. This is a hybrid, not a port: the recompiled
application calls out to real MFC40, MSVCRT40 and Win32, and real MFC calls back
into recompiled code through a `__thiscall` trampoline. Video goes out through
Video for Windows to MCIAVI in the app's own process. Replacing that surface —
`EEUIL10.DLL` first, then MFC40 — is [the roadmap](../ROADMAP.md#2-shrink-the-real-code-surface),
and it is the same work that would make a native Linux or macOS build possible.
Until then, Wine supplies the Windows underneath ([porting.md](porting.md)).

The tools that only parse data — `m20dump`, `strdump`, `datdump`, `ftcdecode`,
`indeodec`, and the Python `mvbtext` — have no such constraint and build
natively anywhere.
