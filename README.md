# Encarta 97 Encyclopedia - Static Recompilation Project

A project to statically recompile Microsoft Encarta 97 Encyclopedia (English, 2-CD edition) into modern, natively-running code for Windows 10/11+.

## Why?

Encarta 97 was a landmark multimedia encyclopedia — the gold standard of digital reference before Wikipedia. It shipped as a Win32 application targeting Windows 95/NT, built with MFC 4.0 and MSVC 4.x. On modern Windows 11 it barely runs due to:

- 16-bit thunking code (`ENCBOOT.EXE`, `ENC16.DLL`, `MMPLAYER.EXE` are NE/Win16)
- Removed WinG/Win32s compatibility layers
- Deprecated multimedia APIs (ACM streams, custom MCI drivers)
- Proprietary media container formats (`.M20`, `.FIF`/`.FTT`, `.MMM`)
- Palette-based 256-color display assumptions
- MSVCRT40.dll / MFC40.DLL dependencies

The goal is a clean, modern C/C++ codebase that runs natively and can serve as a reference implementation for the Encarta data formats.

## Binary Inventory

### Main Executable

| File | Type | Size | Code Size | Description |
|------|------|------|-----------|-------------|
| `ENC97.EXE` | PE32 i386 | 1,715,200 | 0x141000 (~1.3MB) | Main encyclopedia application |

- **ImageBase:** `0x00400000`
- **EntryPoint:** `0x0010DB70`
- **Subsystem:** Windows GUI (2)
- **Sections:** `.text` `.rdata` `.data` `.idata` `.rsrc` `.reloc`
- **Has relocations:** Yes (.reloc section present - good for static recomp)
- **Built with:** MSVC 4.x, MFC 4.0, targeting Win95/NT

### Companion DLLs (PE32)

| File | Size | Code Size | Exports | Role |
|------|------|-----------|---------|------|
| `ENCAPI32.DLL` | 14 KB | 0x1C00 | 30 | Encarta API — CD verification, article lookup, IPC |
| `ENCTITLE.DLL` | 355 KB | 0x45800 | 2 | Title/splash screen, SPAM (content container) interface |
| `DECO_32.DLL` | 134 KB | 0x1D000 | 28 | **FIF image decompressor** — proprietary image codec |
| `EEUIL10.DLL` | 526 KB | 0x45800 | 1,868 | Encarta UI Library — custom MFC widget framework |
| `ENCRES97.DLL` | 2,681 KB | — | 0 | Resource-only DLL (bitmaps, strings, dialogs) |

### Legacy 16-bit Components (NE format — will be replaced)

| File | Description |
|------|-------------|
| `ENCBOOT.EXE` | 16-bit bootstrap launcher |
| `MMPLAYER.EXE` | 16-bit multimedia player |
| `ENC16.DLL` | 16-bit helper / thunking layer |
| `MMX.DLL` | 16-bit multimedia extensions |
| `SETUP16.EXE` / `SETUP32.EXE` | Installer (not needed) |

### System Dependencies

| DLL | Version | Functions Used |
|-----|---------|---------------|
| `MFC40.DLL` | 4.0 | 398 (by ordinal) |
| `MSVCRT40.dll` | 4.0 | 71 |
| `KERNEL32.dll` | — | 107 |
| `USER32.dll` | — | 119 |
| `GDI32.dll` | — | 79 |
| `WINMM.dll` | — | 13 |
| `ADVAPI32.dll` | — | 7 |
| `SHELL32.dll` | — | 3 |
| `comdlg32.dll` | — | 4 |
| `LZ32.dll` | — | 1 |

## Data File Formats

### Content Containers (on both CDs)

| File | Format | Description |
|------|--------|-------------|
| `ENCARTA.M20` | Multimedia Viewer 2.0 | Main encyclopedia content (articles, images) |
| `ATLAS.M20` | MVB 2.0 | World atlas / maps |
| `ANATOMY.M20` | MVB 2.0 | 3D anatomy viewer data |
| `AUDIO1.M20` / `AUDIO2.M20` | MVB 2.0 | Audio clips (CD1/CD2 split) |
| `DICT.M20` | MVB 2.0 | Dictionary |
| `TIMELINE.M20` | MVB 2.0 | Interactive timeline |
| `BIBLIO.M20` | MVB 2.0 | Bibliography |
| `CONSULT.M20` | MVB 2.0 | Research consultant |
| `HILITDLX.M20` | MVB 2.0 | Highlights/features |
| `PICON.M20` | MVB 2.0 | Picture icons/thumbnails |
| `TOPGAL.M20` | MVB 2.0 | Topic gallery |
| `MMBAG.M20` | MVB 2.0 | Multimedia bag |
| `IA1.M20` / `IA2.M20` | MVB 2.0 | Interactive activities |
| `MAXMED1.M20` / `MAXMED2.M20` | MVB 2.0 | Maximum media content |

### Supporting Data

| File | Description |
|------|-------------|
| `ENCARTA.FTI` / `PICON.FTI` | Full-text search index |
| `ENCART97.DAT` | Application configuration |
| `ENC97S.STR` / `ENC97F.STR` | String tables |
| `SEEALSO.DAT` | Cross-reference links |
| `TIMEDB.DAT` / `MTIMEDB.DAT` | Timeline database |
| `PORTIONS.DAT` | Content portions/segments |
| `TOURS.ETO` | Guided tours data |
| `DIET96.DAT` | (CD2) Nutrition database |
| `MINDMAZE.DB` / `MINDMAZE.IDX` | MindMaze trivia game database |
| `E97SPAM*.CMF/MDF/TDF` | SPAM multimedia format files |
| `ANIM.M14` | Animations (Viewer 1.4 format) |

### Multimedia Assets (loose files in `MM/`)

| Type | Format | Count |
|------|--------|-------|
| Videos | `.AVI` (Indeo/Cinepak) | ~85 per CD |
| Music | `.MID` (General MIDI) | ~180 |
| Audio | `.WAV` + `.MMM` pairs | ~50 per CD |

## Key Components to Reimplement

### 1. DECO_32.DLL — FIF Image Decompressor (Priority: HIGH)
Proprietary image format used throughout Encarta. 28 exported functions including:
- `OpenDecompressor` / `CloseDecompressor`
- `SetFIFBuffer` / `ClearFIFBuffer` / `SetFTTBuffer` / `ClearFTTBuffer`
- `DecompressToBuffer` / `DecompressToYUV`
- Resolution/format control (`Get/SetOutputResolution`, `Get/SetOutputFormat`)
- Color table management (`Get/SetOutputColorTable`, `GetFIFColorTable`)
- `GetPhysicalDimensions`, `GetDecoVersion`

### 2. EEUIL10.DLL — Encarta UI Library (Priority: HIGH)
Massive MFC-based custom widget library with 1,868 C++ class exports. Key classes:
- `CRefWnd` / `CRefFrameWnd` / `CRefDialog` — Custom window hierarchy
- `CRefInfo` / `CRefUIManager` — UI state management
- `CFlybar` / `CFlyout` — Custom flyout menus (signature Encarta UI)
- `CRefButton` / `CRefComboBox` / `CRefListBox` / `CRefEdit` — Skinned controls
- `CRefPalMgr` / `CRefFontMgr` / `CRefSoundMgr` — Resource managers
- `CRefPropertySheet` / `CRefPropertyPage` — Settings dialogs
- `CRefToolTipBase` / `CRefActiveToolTip` — Custom tooltips
- `CBrushCache` / `C256Bitmap` — GDI optimization
- `CResourceObject` — Resource loading abstraction

### 3. ENCAPI32.DLL — Encarta API (Priority: MEDIUM)
Small IPC/utility DLL. Key exports:
- CD verification: `fReadVolumeLabel`, `fVerifyEncartaCD`, `fIsEncartaCDPresent`
- Article navigation: `fGetArticleID`, `fGetMainTitle`, `fGetSectionTitle`
- Media lookup: `fGetMediaArticleID`, `fGetMediaTitle`, `fGetMediaClass`
- IPC: `hWndLaunch`, `hWndFindEncarta`, `vDispatchJump`
- ACM stream wrappers: `acmStreamOpen/Close/Convert/Prepare/Unprepare`

### 4. ENCTITLE.DLL — Title Screen / SPAM Interface (Priority: MEDIUM)
- `fGetSpamInterfaces` — Entry point into the SPAM content system
- `fCreateShortcut` — Desktop shortcut creation
- Imports `AM16.dll` and `AMF16.dll` (16-bit SPAM/Viewer thunks)

### 5. M20/MVB Content Parser (Priority: HIGH)
The `.M20` files are Microsoft Multimedia Viewer 2.0 containers — a successor to Windows Help (`.HLP`). Need to reverse engineer or find documentation for:
- Topic/article storage and retrieval
- Embedded image references (FIF format)
- Hotspot/hyperlink encoding
- Full-text index format (`.FTI`)

### 6. ENC97.EXE — Main Application (Priority: HIGHEST)
1.3MB of code. The main application orchestrating everything:
- Article browser and renderer
- Search engine (uses `.FTI` full-text index)
- Atlas/map viewer
- Timeline viewer
- MindMaze trivia game
- Multimedia playback (AVI, MIDI, WAV)
- Print support
- Copy/paste and "word processor export"
- Online update system (yearbook updates via `.YBK` files)

## Architecture Overview

```
┌──────────────────────────────────────────────┐
│                  ENC97.EXE                    │
│           (Main Application)                  │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐     │
│  │ Article  │ │  Atlas   │ │MindMaze  │     │
│  │ Browser  │ │ Viewer   │ │  Game    │ ... │
│  └────┬─────┘ └────┬─────┘ └────┬─────┘     │
│       │             │            │            │
│  ┌────┴─────────────┴────────────┴───────┐   │
│  │         Content Engine (MVB 2.0)       │   │
│  └────┬───────────────────────┬──────────┘   │
├───────┼───────────────────────┼──────────────┤
│  ┌────┴─────┐           ┌────┴─────┐        │
│  │EEUIL10   │           │ DECO_32  │        │
│  │(UI Lib)  │           │(FIF Img) │        │
│  └──────────┘           └──────────┘        │
├──────────────────────────────────────────────┤
│  ENCAPI32    │  ENCTITLE   │  ENCRES97       │
│  (API/IPC)   │  (SPAM)     │  (Resources)    │
├──────────────────────────────────────────────┤
│  MFC40.DLL  │  MSVCRT40   │  Win32 APIs     │
└──────────────────────────────────────────────┘
         │              │
    ┌────┴──┐     ┌─────┴─────┐
    │ CD 1  │     │   CD 2    │
    │.M20   │     │  .M20     │
    │.AVI   │     │  .AVI     │
    │.MID   │     │  .MID     │
    └───────┘     └───────────┘
```

## Recompilation Strategy

### Phase 1: Foundation & Data Formats — ✅ substantially complete
- [x] Set up build system (CMake + MSVC, Win32 target)
- [x] Reverse engineer DECO_32.DLL (FTC/FIF image codec) — **fully statically
      recompiled to native C** (all 28 exports, 137-function closure; see below)
- [x] Reverse engineer M20/MVB 2.0 container format (`m20dump`: B-tree walk,
      leaf-page parse, extract; `.FSM` entries are uncompressed `FTC\0` images)
- [x] Write standalone content extraction tools (`m20dump`, `decooracle`, `recomp`)
- [x] End-to-end real-content pipeline: ISO → `m20dump -x` → decode real
      `PICON.M20` images to full-colour PNG
- [x] **Drop the runtime DLL dependency** — `recomp_decode_standalone`
      reconstructs the codec's data image from a generated blob; all 5 picons
      decode byte-exact with **no `DECO_32.DLL` file**
- [ ] Finish format docs; handle the `"FIFF"` FIF variant + non-full-res scaling

### Phase 2: UI Framework
- [ ] Map EEUIL10.DLL's 1,868 exports to class hierarchy
- [ ] Reimplement core widget classes using modern Win32/GDI
- [ ] Replace 256-color palette logic with 32-bit rendering
- [ ] Reimplement CRefUIManager and theming system

### Phase 3: Core Application — 🚧 lifter scales to the whole app; entry boots
The strategy here is **mechanical static recompilation** (x86→C), the same
approach proven on DECO_32, rather than hand-reimplementation:
- [x] Static disassembly + function map of ENC97.EXE (IDA Pro idalib, 7,326 fns)
- [x] **Whole binary lifts to compilable C** — all 7,326 functions, 0 unhandled
      opcodes (`enc97_full.c`, ~452k lines, compiles clean)
- [x] Dispatch table validated at scale — **818 functions differential-match the
      real originals**, byte-exact
- [x] **All 914 imports wire to real code** (MFC40/MSVCRT40 ship in SysWOW64; the
      "MFC40 wall" was a myth) — `LoadLibrary(ENC97.EXE)` itself succeeds
- [x] Lifter handles real position-dependent / SEH code: **`fs:` segment access**
      and **address-immediate relocation** (`.reloc`-driven)
- [x] **The lifted entry point boots the app** — runs CRT init, `AfxWinMain`,
      ENC97's `InitInstance`, to a clean `exit`; `_initterm` routed so 182
      static-initializers run as lifted code
- [x] **Bidirectional lifted↔real boundary** — import trampoline (lifted→real)
      + real→lifted `__thiscall` trampoline (real MFC virtual-dispatch → lifted),
      with vtable routing (10k+ slots)
- [x] **Full vtable routing runs clean** — the heap corruption was two harness
      bugs, not a lift bug: `call_machine` didn't seed `ebp` (so unlifted
      frameless SEH/dtor funclets addressed the host's frame) and wasn't
      reentrant (a nested call clobbered the outer's saved `esp`)
- [x] **The recompiled boot reaches Encarta's own UI** — all 10,432 fn-pointer
      slots routed, CRT + MFC init + `InitInstance` lifted, app dialog on screen
- [x] **The application body runs recompiled** — `InitInstance` returns TRUE and
      MFC calls the app's `Run()` lifted: palette, `EEUIL10` UI-library init,
      startup sound, window classes. 2,200 dispatched calls, 250 real→lifted
      virtual dispatches; `R2L_PASSTHRU` (real bodies) reaches the same point
- [x] Lifter: `.reloc`-marked **displacements** inside memory operands are now
      GVA-wrapped too, not just address immediates (`mov dl,[ecx+0x56d902]`)
- [x] **Encarta 97 starts** — `ENC97_PROFILE` answers the `97Options` profile
      lookups (`CodePath`/`DATPath`/`BookPath`) the app's Setup would have
      written, so with CD1 mounted it loads content and opens its main window,
      "Microsoft Encarta 97 Encyclopedia", without installing anything
- [x] Lifter: an indirect `call` now resolves its target **before** pushing the
      return address (`call [esp+0x18]` was reading one slot off)
- [x] `LIFT_LO`/`LIFT_HI` — bisect *which functions run lifted* to pin a bad lift,
      the function-level twin of the slot-routing bisect
- [ ] Article browser / search / atlas / MindMaze, with the whole body lifted
- [ ] Article browser / search / atlas / MindMaze (emerge from the lifted body)

See `tools/recomp/README.md` for the full ENC97 recompilation writeup.

### Phase 4: Multimedia & Polish
- [ ] Replace ACM stream wrappers with modern audio APIs
- [ ] Update AVI playback (Indeo codec → FFmpeg/native)
- [ ] MIDI playback
- [ ] Remove CD-check / volume label verification
- [ ] Support reading content from local directory (no CD needed)
- [ ] Testing and compatibility

## Building

```bash
# Configure (requires Visual Studio 2022, Win32 target)
cmake -B build -G "Visual Studio 17 2022" -A Win32

# Build all tools
cmake --build build --config Release

# Build a specific tool
cmake --build build --config Release --target ftcdecode
cmake --build build --config Release --target m20dump
```

### Tools

| Tool | Directory | Description | Status |
|------|-----------|-------------|--------|
| `recomp` | `tools/recomp/` | **Static recompilation of the DECO_32 FTC codec** (x86→C) | **Working — pixel-exact** ([details](tools/recomp/README.md)) |
| `decooracle` | `tools/decooracle/` | Faithful DECO_32.DLL bridge → full-colour FTC; ground-truth oracle | **Working — perfect colour** |
| `ftcdecode` | `tools/ftcdecode/` | Clean-room FTC/FTT/FIF image decoder | Working (FTC grayscale, FTT/FIF perfect) |
| `m20dump` | `tools/m20dump/` | M20/MVB 2.0 container extractor | Working |
| `mvbtext` | `tools/mvbtext/` | Encarta article title/text extractor (MVB 2.0) | Titles ✓; topic bodies partial ([details](tools/mvbtext/README.md)) |
| `encextract` | `tools/encextract/` | End-to-end pipeline: disc → decoded image gallery + titles + HTML | Working ([details](tools/encextract/README.md)) |
| `strdump` | `tools/strdump/` | STR string table dumper | Working |
| `spamdump` | `tools/spamdump/` | SPAM multimedia format dumper | Working |
| `datdump` | `tools/datdump/` | DAT configuration dumper | Working |
| `fifdecode` | `tools/fifdecode/` | Old DLL bridge (wrong export signatures) | Superseded by `decooracle` |

### Static Recompilation (`tools/recomp`)

**DECO_32.DLL — done.** The proprietary FTC image codec has been **fully
statically recompiled** — **all 28 exports** mechanically translated from x86
(incl. x87 FPU) to native C by a purpose-built lifter (`lift.py`, capstone-based),
a **137-function closure** with zero unhandled opcodes. Validated
**byte-identical** to the original DLL, with **no original code executed**. It now
also runs with **no `DECO_32.DLL` file at all** (`recomp_decode_standalone`
reconstructs the data image from a generated blob). See
[`tools/recomp/README.md`](tools/recomp/README.md).

**ENC97.EXE — the whole 1.3 MB MFC app lifts, and the recompiled entry boots it.**
All **7,326 functions** lift to compilable C (0 unhandled opcodes); the dispatch
table is validated at scale (**818 functions differential-match** the originals);
all **914 imports wire to real code** (MFC40/MSVCRT40 ship in SysWOW64). The
lifter gained `fs:`/SEH and `.reloc`-driven address-immediate relocation, so the
**lifted entry point boots the application** — CRT init → `AfxWinMain` →
`InitInstance` → clean `exit`, with `_initterm` routed so 182 static-initializers
run lifted. A **real→lifted `__thiscall` trampoline** (the inverse of the import
trampoline) lets real MFC virtual-dispatch into lifted code, and with all 10,432
function-pointer slots routed the boot now runs clean to **Encarta's own
dialog** — the app's UI, drawn by recompiled code.

ENCAPI32.DLL is also lifted and validated against the real Win32 boundary
(`fGetArticleID`).

Both FTC encoding modes found in real content decode correctly:

| Mode | Header bytes | Share of PICON | Result |
|------|--------------|----------------|--------|
| self-contained | `01 01 02 01` | ~29% | **pixel-exact** to the DLL |
| FTT-referenced | `04 03 04 01` | ~71% | **clean full colour** (recomp is *more correct* than the DLL bridge, which speckles chroma under standalone setup) |

How it was built: a faithful DLL-bridge **oracle** (`decooracle`) established
ground-truth output + CRC baselines and a per-function call **tracer**
(`decotrace`); the codec was then lifted leaf-up and differentially validated
against the oracle at every step (the VLC reader alone passed 3,000,000
fuzz trials with zero mismatches).

### Real-content image pipeline

```bash
# 1. mount an Encarta ISO  (PowerShell: Mount-DiskImage CD1ENC97ENC.iso)
# 2. extract raw images from a container (use -x; the FTC entries are NOT
#    LZ77-compressed, so -d would corrupt them)
m20dump -x "G:\ENCYC97\PICON.M20" -o out_dir
# 3. decode a real image to PNG (auto-loads any referenced FTT from the same dir)
recomp_decode out_dir\T000009D.FSM "" image.png
```

`PICON.M20` holds **11,348 FTC images** (`.FSM`) + 49 shared fractal transform
tables (`.FTT`); all decode to correct full colour through the recompiled codec.

### FTC Decoder (`ftcdecode`)

Clean-room image decoder for Encarta 97's FTC (Fractal Transform Codec), FTT (raw pixel), and FIF (container) formats. Auto-detects format by magic bytes.

```bash
# Decode FTC (fractal compressed) to grayscale BMP
ftcdecode input.ftc output.bmp

# Decode FTT (raw uncompressed) to BMP — perfect quality
ftcdecode input.ftt output.bmp

# Extract image from FIF container — scans for embedded FTT/FTC
ftcdecode input.fif output.bmp

# Show header info
ftcdecode -i input.ftc

# Decode with debug output
ftcdecode -d input.ftc output.bmp
```

**Decode pipeline status:**
- [x] FTC header + sub-header parsing (28 + 39 bytes)
- [x] Sub-header context/parameter extraction (small mode)
- [x] LSB-first bitstream reader
- [x] 3-pass block assignment (green/skip/blue/red states)
- [x] 24-bit block decoding (7 scale + 14 offset + 3 opcode)
- [x] 4×4 superblock scan order (padded grid)
- [x] 16-bit scale table computation (word0=6 divide-by-10 formula)
- [x] FTT raw decode — **perfect quality** grayscale output
- [x] FIF container decode — **perfect quality**, extracts embedded FTT/FTC sub-images
- [x] FTC flat-fill decode — **recognizable grayscale** for all test files
- [x] Chroma scale table (word0=8 divide-by-16, separate from luma word0=6)
- [x] **FTC full colour — SOLVED** (via `decooracle` and the `recomp` static
      recompilation; the clean-room `ftcdecode` chroma path is superseded by the
      recompiled codec)

## Tools Needed

- [Ghidra](https://ghidra-sre.org/) or IDA Pro — for disassembly of PE32 binaries
- [Resource Hacker](http://www.angusj.com/resourcehacker/) — for extracting resources from ENCRES97.DLL
- Visual Studio 2022 (MSVC) — for building tools (Win32 target)
- CMake 3.16+ — build system
- Python 3 + `pefile` + `capstone` — for PE analysis and the x86→C lifter
  (`tools/recomp/lift.py`)
- IDA Pro (idalib, headless) / Ghidra — function boundaries + decompilation

## Legal

This project contains no copyrighted Microsoft code or content. It is a clean-room reimplementation effort. You must own a legitimate copy of Encarta 97 Encyclopedia to use the content files.

## Status

**Phase 1 (Data Format RE) — substantially complete; the DECO_32 image codec is
fully recompiled. Next up: remaining Phase-1 polish, then Phase 2/3.**

- [x] Identify all executables and DLLs (PE32 vs NE/16-bit)
- [x] Catalog PE sections, imports, exports for all 32-bit modules
- [x] Map data file formats and multimedia assets
- [x] Document architecture and component relationships
- [x] Ghidra/IDA disassembly of DECO_32.DLL — all functions mapped
- [x] M20/MVB 2.0 container parser + extractor (`m20dump`)
- [x] FTT raw decoder + FIF container decoder — **perfect quality**
- [x] FTC image decoder (clean-room) — luma/grayscale recognizable
- [x] **FTC full-colour decode — SOLVED** (`decooracle` faithful DLL bridge)
- [x] **DECO_32 statically recompiled (x86+x87→C) — all 28 exports, 137-function
      closure, byte-exact, no original code executed** (`tools/recomp`)
- [x] **End-to-end real-content pipeline** — decode real `PICON.M20` images
      (both FTC modes) to full-colour PNG
- [ ] Clean-room regeneration of the codec's constant tables (drop DLL-data dep)
- [ ] Full M20/MVB format documentation; `"FIFF"` FIF variant + scaling paths
- [ ] **Begin Ghidra/IDA disassembly of ENC97.EXE** (main application) — Phase 3
- [ ] Map EEUIL10.DLL UI class hierarchy — Phase 2
