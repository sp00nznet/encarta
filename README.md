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

### Phase 1: Foundation & Data Formats
- [ ] Set up build system (CMake + MSVC or MinGW)
- [ ] Reverse engineer DECO_32.DLL (FIF image codec) — smallest, self-contained
- [ ] Reverse engineer M20/MVB 2.0 container format
- [ ] Write standalone content extraction tools
- [ ] Document all file formats

### Phase 2: UI Framework
- [ ] Map EEUIL10.DLL's 1,868 exports to class hierarchy
- [ ] Reimplement core widget classes using modern Win32/GDI
- [ ] Replace 256-color palette logic with 32-bit rendering
- [ ] Reimplement CRefUIManager and theming system

### Phase 3: Core Application
- [ ] Static disassembly of ENC97.EXE (IDA Pro / Ghidra)
- [ ] Identify and annotate major subsystems
- [ ] Reimplement article browser and renderer
- [ ] Reimplement search engine
- [ ] Reimplement atlas viewer
- [ ] Reimplement MindMaze game

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
| `ftcdecode` | `tools/ftcdecode/` | FTC/FTT image decoder | **Working** (FTC grayscale + FTT raw) |
| `m20dump` | `tools/m20dump/` | M20/MVB 2.0 container extractor | Working |
| `fifdecode` | `tools/fifdecode/` | DLL bridge to DECO_32.DLL | Broken on Win11 (DEP) |
| `strdump` | `tools/strdump/` | STR string table dumper | Working |
| `spamdump` | `tools/spamdump/` | SPAM multimedia format dumper | Working |
| `datdump` | `tools/datdump/` | DAT configuration dumper | Working |

### FTC Decoder (`ftcdecode`)

Clean-room image decoder for Encarta 97's FTC (Fractal Transform Codec) and FTT (raw pixel) formats. Based on reverse engineering of `DECO_32.DLL`.

```bash
# Decode FTC (fractal compressed) to grayscale BMP
ftcdecode input.ftc output.bmp

# Decode FTT (raw uncompressed) to BMP — perfect quality
ftcdecode input.ftt output.bmp

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
- [x] FTT raw decode — **perfect quality** grayscale output (uncompressed format)
- [x] FTC flat-fill decode — **produces recognizable grayscale images** for all test files
- [x] Chroma scale table (word0=8 divide-by-16, separate from luma word0=6)
- [ ] Color output — chroma blocks decoded but high spatial noise; DLL pixel transform uses 1024-byte LUT + arithmetic coder, needs deeper RE
- [ ] Fractal IFS iteration — DLL applies one-shot transform via LUT, not iterative convergence as initially assumed

## Tools Needed

- [Ghidra](https://ghidra-sre.org/) or IDA Pro — for disassembly of PE32 binaries
- [Resource Hacker](http://www.angusj.com/resourcehacker/) — for extracting resources from ENCRES97.DLL
- Visual Studio 2022 (MSVC) — for building tools (Win32 target)
- CMake 3.16+ — build system
- Python 3 + `pefile` — for PE analysis scripts

## Legal

This project contains no copyrighted Microsoft code or content. It is a clean-room reimplementation effort. You must own a legitimate copy of Encarta 97 Encyclopedia to use the content files.

## Status

**Current Phase: 1 — Data Format Reverse Engineering**

- [x] Identify all executables and DLLs (PE32 vs NE/16-bit)
- [x] Catalog PE sections, imports, exports for all 32-bit modules
- [x] Map data file formats and multimedia assets
- [x] Document architecture and component relationships
- [x] Ghidra disassembly of DECO_32.DLL — key functions mapped
- [x] M20 container extraction tool
- [x] FTT image decoder — **perfect quality** raw pixel decode
- [x] FTC image decoder — **luma channel producing recognizable images** (flat-fill mode)
- [ ] FTC decoder — color output (pixel transform uses LUT + arithmetic coder, needs deeper RE)
- [ ] FIF container format decoder (wraps FTC frames)
- [ ] Begin Ghidra/IDA disassembly of ENC97.EXE
