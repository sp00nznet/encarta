# Roadmap

Where it stands, what is planned, and in what order. The short version -
what works today - is in the [README](../README.md); this is the long one.

Where it stands: the application body is recompiled C; MFC40, MSVCRT40,
EEUIL10 and Windows itself are still real code underneath. That is a
working program, not yet a port. The distance between the two is this file.

## Done so far

- [x] Identify all executables and DLLs (PE32 vs NE/16-bit)
- [x] Catalog PE sections, imports, exports for all 32-bit modules
- [x] Map data file formats and multimedia assets
- [x] Ghidra/IDA disassembly of DECO_32.DLL — all functions mapped
- [x] M20/MVB 2.0 container parser + extractor (`m20dump`)
- [x] FTT raw decoder + FIF container decoder — **perfect quality**
- [x] **FTC full-colour decode — SOLVED**; **DECO_32 statically recompiled**
      (all 28 exports, byte-exact, no original code executed, no DLL needed)
- [x] **ENC97.EXE lifts whole** (7,326 fns) and **the application runs** as
      recompiled code, hybrid against real MFC

## Recompilation Strategy

### Phase 1: Foundation & Data Formats — ✅ substantially complete
- [x] Set up build system (CMake + MSVC, Win32 target)
- [x] Reverse engineer DECO_32.DLL (FTC/FIF image codec) — **fully statically
      recompiled to native C** (all 28 exports, 137-function closure —
      [`tools/recomp`](../tools/recomp/README.md))
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

### Phase 3: Core Application — ✅ the app runs as recompiled code
The strategy here is **mechanical static recompilation** (x86→C), the same
approach proven on DECO_32, rather than hand-reimplementation:

**The lift**
- [x] Static disassembly + function map of ENC97.EXE (IDA Pro idalib, 7,326 fns)
- [x] **Whole binary lifts to compilable C** — all 7,326 functions, 0 unhandled
      opcodes (`enc97_full.c`, ~452k lines, compiles clean)
- [x] Dispatch table validated at scale — **974 functions differential-match the
      real originals**, byte-exact
- [x] **All 914 imports wire to real code** (MFC40/MSVCRT40 ship in SysWOW64; the
      "MFC40 wall" was a myth)
- [x] Position-dependent / SEH code: **`fs:` segment access**, and `.reloc`-driven
      relocation of both address **immediates** and **displacements inside memory
      operands** (`mov dl,[ecx+0x56d902]`)
- [x] Indirect `call` resolves its target **before** pushing the return address —
      a real `call [esp+0x18]` reads its target with the pre-push `esp`
      (12,703 call sites; this one cost a jump to address 0)

**The hybrid boundary**
- [x] **Bidirectional lifted↔real** — import trampoline (lifted→real) plus a
      real→lifted `__thiscall` trampoline, so real MFC virtual-dispatch lands in
      recompiled code; **10,432 function-pointer slots routed**
- [x] `_initterm` routed so 182 static initializers run lifted
- [x] Three correctness rules the boundary trampoline has to obey, each found the
      hard way: seed **`ebp`** (MSVC's frameless SEH/dtor funclets address the
      *caller's* frame), be **reentrant** (a nested call must not clobber the
      outer's saved `esp`), and capture **`edx`** (64-bit returns come back in
      `edx:eax`)

**Running the thing**
- [x] The app gets its own command line (the lifted CRT parses `_acmdln`, which
      is otherwise the *harness* process's)
- [x] `ENC97_PROFILE` answers the `97Options` profile lookups
      (`CodePath`/`DATPath`/`BookPath`) that Setup would have written — so with
      CD1 mounted the app finds its content **without installing anything**
- [x] **Encarta 97 runs**: toolbar, article text, and its illuminated-manuscript
      artwork decoded and drawn ([screenshot](encarta97-running.png))
- [x] **Driven interactively** — article browsing, search, dictionary, the media
      archive, audio and MindMaze all work from recompiled code
- [x] **Indeo 3.2 decoder** — the 16-bit `IR32.DLL` driver statically
      recompiled and **byte-exact**: 68 of 68 first frames decode to 100.00%
      identical pixels against FFmpeg, and the RGB frame the codec itself
      returns matches at correlation 1.0000
      ([`tools/indeo`](../tools/indeo/README.md))
- [x] **Video codec bridged to VFW** — `ir32vfw.dll` registers the
      recompiled decoder with `ICInstall`, and the ENC97 harness confirms VFW
      finds it in-process. 68 of 68 clips decode through `ICM`
- [x] **Clips play in the app** — the Media Gallery lists all 68 videos and
      plays them ([screenshot](encarta97-video.png)); no other Indeo
      decoder exists on the machine, so those frames are the recompiled
      codec's. 10 fps content, 59 fps decode
- [x] SPAM media (article pictures) — `AM16.DLL`/`AMF16.DLL` beside the app
- [ ] Shrink the real-code surface: replace MFC40/EEUIL10 with native equivalents

**Debugging tools** (in `recomp_enc97_run`, see its header for the full list)
- `R2L_LO`/`R2L_HI` — bisect *which vtable slots are routed* to lifted code
- `LIFT_LO`/`LIFT_HI` — bisect *which functions run lifted* to pin a bad lift
- `R2L_PASSTHRU` / `R2L_STUB` / `R2L_REAL` — an isolation ladder that says
      whether a failure is in the lift, the slot rewrite, the calling convention,
      or the stack switch
- `MSGBOX_LOG`, `REG_LOG`, `WATCH=va,...`, `RUN_TRACE`, `R2L_HEAPCHECK`

See `tools/recomp/README.md` for the full ENC97 recompilation writeup.

### Phase 4: Multimedia & Polish
- [ ] Replace ACM stream wrappers with modern audio APIs
- [x] AVI playback — done, and not the way this line assumed: the CD's own
      16-bit Indeo driver was recompiled and handed back to Video for
      Windows, so no FFmpeg or reimplementation was needed
- [ ] MIDI playback
- [x] CD check / volume label — answered per-process for one drive letter
      ([`tools/localcontent`](../tools/localcontent/README.md))
- [x] Content from a local directory — mirror the disc once and run from
      it, **with no disc in the machine**: 26 file operations on the copy and
      none anywhere else
- [ ] Testing and compatibility

## What is next

**1. Prove it, and stop it regressing.** The app boots to the article view —
that is one code path. Everything else (search, atlas, timeline, MindMaze,
media playback) is untested lifted code, and each will surface its own lift
bugs the way `sub_4BB6F0` did.
- [ ] Drive the UI and fix what falls out, using the `R2L_LO/HI` and
      `LIFT_LO/HI` bisects — they turn "it crashed 30,000 calls in" into a
      named function in ~14 runs
- [x] **A scripted regression run** — `py tools/regress.py` checks the codec,
      the decoder's byte-exactness, both video paths, the phrase encoding,
      article text, the media list and the app reaching its window. Fast set
      ~4s, `--full` ~30s; a missing CD reports SKIP, never a pass
- [x] **Differential validation raised to 974/7,326** — the 818 no-write
      leaves plus 156 that write memory, those compared on the buffer as
      well as `eax`. 2 float leaves held back, 1 indeterminate
- [x] **The sweep made deterministic** — it passed 3 runs in 4 and failed the
      fourth on `sub_50BE80`, a distance test reading its arguments as doubles
      from a stack the harness never filled. Now filled with a known pattern
      and each side run twice under two of them; 15 runs, same number
- [~] **Past the leaves: the callee policy is measured, not yet running.**
      5,316 functions call something. The two barriers are entangled — against
      a 1,059 baseline, stubbing imports alone reaches 1,127 and tolerating
      indirect calls alone 1,196, but both together reach 3,292 loop-free
      functions, 1,243 of which call. What blocks it is that the import stub
      must pop each callee's stdcall arguments, and 785 of 914 counts could not
      be read from the API epilogues. Next: count the pushes at ENC97's own
      call sites

**2. Shrink the real-code surface.** This is the actual work of becoming a port,
in cost order:
- [ ] **Lift `EEUIL10.DLL`** (1,868 exports) — it is PE32, the same lifter
      applies, and it is *Encarta's own* UI library. The single biggest step
      toward the app being entirely our code
- [ ] `ENCTITLE.DLL` — blocked on its 16-bit `AM16`/`AMF16` thunks
- [ ] `MFC40.DLL` — 398 imports by ordinal. Keep it real (it works) or
      reimplement the used subset; a decision, not an obligation
- [x] Run from a local content directory — `mirror-cd.ps1` copies the
      disc, `run-encarta.ps1` runs from the copy
      ([`tools/localcontent`](../tools/localcontent/README.md))

**3. The 1997 assumptions.** Palette-based 256-colour rendering, GDI, ACM audio,
MIDI. Replace with modern equivalents once the code above is ours to change.
Indeo is already handled - not by replacing it, but by recompiling the driver
the disc ships (all 68 clips are Indeo 3.2; there is no Cinepak on CD1).

**4. Then the interesting question: 64-bit and non-Windows.** Today everything
is 32-bit because lifted registers hold real host pointers. Both need the same
thing — a memory model that isn't "the register is the address".

**Formats** — documented in **[docs/FORMATS.md](FORMATS.md)**
- [x] M20/MVB 2.0 container, B-tree directory, internal file layout
- [x] **`|Phrases` phrase dictionary decoded** — 1,808 entries, byte-exact
      against the size its own header declares
- [x] `|TTLBTREE` titles (31,517), `.RLE` baggage (plain BMPs), FTC/FTT/FIF
- [x] **Topic entries are LZ77** — same encoder as `|Phrases`; `mvbtext prose`
      decompresses them and reads real article text out
- [x] **Phrase references decoded** — one byte for the 32 fragments
      (`0x80-0x9F`), two for the rest (`0xA0-0xBF`: index
      `((b & 0x0F) << 8) | next`, plus a space when `b & 0x10`). Frequency
      settled it: the commonest codes are `the`, `of `, `and`, `in ` in order.
      Article prose now reads as prose
- [x] **Topic records split** — records are NUL-separated (five NULs open a
      heading, three close it) and the media list is read as data: 1,001
      references across 120 topics
- [ ] **The remaining structure records** — 138 of 240 are still recognised
      only by their byte profile, and the entry header is unparsed, so
      `topic_stream` finds the LZ77 stream by trying every offset
- [ ] Clean-room regeneration of the codec's constant tables (drop DLL-data dep)

**Upstream** — three lifter bugs the Unicorn differential test found while
validating the 16-bit Indeo lift, none of which is read on the decode path but
all of which are real, and belong back in
[pcrecomp](https://github.com/sp00nznet/pcrecomp):
- [ ] shifts do not set OF
- [ ] `mul`/`imul` write flags x86 leaves undefined
- [ ] 16-bit `adc`/`sbb` lose CF when `src + cf` wraps
- [ ] `"FIFF"` FIF variant + non-full-resolution scaling paths
