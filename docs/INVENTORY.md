# What is on the discs

An inventory of Encarta 97's binaries and data files, and how they fit
together. For how the formats themselves are laid out, see
[FORMATS.md](FORMATS.md); for what has been recompiled so far, see the
[README](../README.md).

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

### Content Containers

The set spans the two discs. CD1 carries 14 of them; `AUDIO2`, `IA2` and
`MAXMED2` are on CD2.

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

Counted on CD1, not estimated. CD2 has not been surveyed the same way, so no
per-CD figure is claimed here.

| Type | Format | Count on CD1 |
|------|--------|--------------|
| Videos | `.AVI` | 68 |
| Music | `.MID` (General MIDI) | 14 |
| Narration | `.WAV` | 58 |
| Narration sidecars | `.MMM` | 56 |

Every one of the 68 clips is **Indeo 3.2** (`IV32`) - there is no Cinepak on
CD1, which an earlier version of this table claimed. That was checked properly:
the demuxer walks all 68 and validates its header invariants on all 9,373
frames ([`tools/indeo`](../tools/indeo/README.md)).

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
