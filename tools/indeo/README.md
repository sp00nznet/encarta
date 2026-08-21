# indeodec — Indeo 3 (IV32) video

Encarta 97's 68 video clips are **Indeo 3.2**. Microsoft removed the Indeo
codecs from Windows years ago, and the CD ships only the 16-bit driver
(`AAMSSTP\SYSTEM16\IR32.DLL`, an NE binary), which a 32-bit process cannot
load. So the recompiled application runs, and its videos do not.

This is the start of our own decoder.

## Method

The DECO_32 image codec was cracked with an **oracle**: run the original, diff
against it, and lift leaf-up. Indeo cannot work that way — the only Indeo
binaries on the disc are 16-bit and will not load in-process.

So instead:

- **The format is derived from the bitstream.** Every field below was worked
  out by reading the bytes of the shipping clips and checking each invariant
  across every frame of every clip. Fields not yet understood are named `unk*`
  rather than guessed at.
- **ffmpeg is used only as an external reference**, run as a program to emit
  raw YUV to compare our output against. Its decoder is LGPL and this project
  is MIT; none of its code is used here, and none should be.
- **The parser fails loudly.** `iv3_parse_header` returns the first invariant
  that breaks, so a stream that is not what we think it is says so instead of
  decoding into nonsense. That is what turned up the null frames below.

## Verified

Checked across **all 68 clips — 9,373 frames, zero violations.**

### Container

Ordinary RIFF/AVI, with one trap. The spec's video chunk suffixes are `dc` and
`db`; these clips also use **`iv`** and **`32`**. A demuxer that whitelists
`dc`/`db` silently reports *zero frames* on most of the collection — and zero
frames trivially passes any per-frame check, which is a very comfortable way to
be wrong. `avi_walk` therefore excludes the non-video suffixes (`wb` audio,
`tx` text, `pc` palette) and accepts the rest.

Some clips also group frames into `LIST 'rec '` records rather than placing
chunks directly in `movi`.

### Frame header — 48 bytes, little-endian

| Off | Type | Field | Notes |
|-----|------|-------|-------|
| 0  | u32 | `frame_number` | sequential from 0 |
| 4  | u32 | — | always 0 |
| 8  | u16 | `checksum` | varies per frame |
| 10 | u16 | **signature** | always `0x4652` |
| 12 | u32 | `data_size` | payload bytes, ≤ chunk size |
| 16 | u32 | `flags` | low half always `0x0020`; high half cycles per GOP |
| 20 | u32 | `unk20` | tracks frame content |
| 24 | u16 | — | always `0x1200` |
| 26 | u16 | `unk26` | |
| 28 | u16 | **height** | agrees with the AVI stream header |
| 30 | u16 | **width** | |
| 32 | u32 | `plane_off[0]` | largest — the luma plane |
| 36 | u32 | `plane_off[1]` | |
| 40 | u32 | `plane_off[2]` | always 48, i.e. straight after the header |
| 44 | u32 | — | always 0 |

Plane offsets are from the start of the frame and strictly ascending as
`[2] < [1] < [0]`, so each plane's length is the gap to the next (and the last
runs to `data_size`). On a keyframe the luma is by far the largest — ~8.7 KB
against ~670 bytes per chroma plane on a 216×192 clip.

### Null frames

`data_size == 48` — header, no payload — means **hold the previous picture**.
Their plane offsets are stale values left over from an earlier frame and must
be ignored. A handful of frames use a short stub chunk instead of a zero-length
one, which amounts to the same thing.

These are common (136 across the collection), and an over-strict parser rejects
them as corrupt. They are not.

### Fixed table

The 16 bytes at offset 48 are **byte-identical in every frame of every clip**:

```
02 14 26 38 4a 5c 6e 7f 82 94 a6 b8 ca dc ee ff
```

A 16-entry ramp in steps of ~18, symmetric about the midpoint — the shape of a
dequantisation/reconstruction table rather than picture data. Plane data starts
after it.

### Plane regions

Three regions per coded frame, from the offsets above. On a 216x192 clip the
largest is ~8.7 KB and the other two ~670 bytes each, against pixel counts of
41,472 luma and 2,592 per chroma plane - so `plane_off[0]` is the luma.

`plane_off[2]` is always 48 and its first 16 bytes are always the constant
table. Beyond that the plane interior is only partly characterised. One
recurring 16-byte block turns up at the start of many luma regions and
correlates with delta frames (45 of 54) but not cleanly - three keyframes carry
it too, so it is not simply a plane-type marker and is recorded here as an
observation, not a conclusion.

## Scoring

`ffmpeg` supplies the reference; `indeodec -r` walks it frame by frame against
our output and reports PSNR, so any change to the decoder is measurable rather
than a matter of opinion:

```bash
ffmpeg -i clip.avi -pix_fmt yuv410p -f rawvideo ref.yuv
indeodec -r clip.avi ref.yuv
```

The current "decoder" is deliberately the dumbest thing consistent with what is
established - null frames hold the previous picture, everything else is flat
mid-grey. On the sample clip that scores **17.50 dB**. It is not a decoder, it
is the number to beat.

## Not done

**The pixel decoding.** Indeo 3 codes each plane as a tree of cells with
vector-quantised deltas, and none of that is implemented yet. That is the bulk
of the work and it is untouched.

Order of attack:

1. Plane header and the cell-splitting tree.
2. The VQ delta tables, of which the ramp above is likely one.
3. Intra (keyframe) cells, checked against the reference YUV.
4. Inter (delta) cells and motion vectors.

## Lifting IR32.DLL

`IR32.DLL` **is** the decoder; it is simply a 16-bit NE binary, so it cannot be
loaded in-process the way `DECO_32.DLL` was. It can be lifted instead, which is
the method that already produced a byte-exact `DECO_32`. First question: does
the 16-bit decoder in pcrecomp understand this binary?

`ir32_scan.py` answers it - a linear sweep over every code segment, reporting
what decodes and what does not:

```bash
py tools/indeo/ir32_scan.py H:\AAMSSTP\SYSTEM16\IR32.DLL
```

**99.21%** - 114,386 of 115,298 bytes, 45,996 instructions across all 40 code
segments. Most segments decode completely; the weakest are the big compute
segments (3 at 97.8%, 40 at 98.6%).

### Shape of the binary

| | |
|---|---|
| Segments | 47 (40 code, 7 data), 115,298 bytes of code |
| Relocations | 943 (112 internal, 831 imports) |
| Imports | WIN87EM, KERNEL, GDI, USER, MMSYSTEM |
| Entry points | `DRIVERPROC`, `LIBMAIN`, `WEP`, `IR32`, two dialog procs |

Segments **3** (20.8 KB, 4 relocations) and **40** (18 KB, none) make no calls
at all - leaf compute, which is the shape of pixel inner loops. Together they
are a third of the code.

There is no shortcut through the call graph: the closure from `DRIVERPROC`
reaches 39 of 40 segments (112 KB of 115 KB), because the VFW driver entry
dispatches everything - decompress, compress and configuration alike. Narrowing
to just the decode path needs the ICM message dispatch understood first.

### What the missing fraction was

Not random, and not what it first looked like. The initial survey blamed the
*first byte* of each failed instruction, which is a prefix - so handled
prefixes appeared unsupported and the real culprit was hidden. Corrected, the
answer was specific:

```
64 89 2E 06 00        mov fs:[6],bp      -> FAIL (len 1)
67 64 89 2E 06 00     mov fs:[dword 6],bp-> FAIL
67 8B 06 34 12        mov ax,[dword]     -> ok      (addr32 alone is fine)
89 2E 06 00           mov [6],bp         -> ok      (plain is fine)
```

`decode16.py` handled ES/CS/SS/DS overrides and the `66`/`67` size prefixes,
but **not FS/GS** (`0x64`/`0x65`). Those are 386 additions that 16-bit code is
not supposed to need - except IR32 keeps decoder state in an FS-addressed block
and reaches it constantly. With `0x64` unrecognised the sweep resynced one byte
in, and the resulting misalignment then blamed perfectly ordinary `mov`s
(`8B`, `8A`, `A1`) that happened to follow. Every one of those decodes fine in
isolation.

Two prefix cases in `pcrecomp/tools/disasm/decode16.py` fixed it:

| | before | after |
|---|---|---|
| whole DLL | 99.21% | **99.70%** |
| seg 2 | 95.0% | **100%** |
| seg 3 (main compute) | 97.8% | **100%** |

What still fails is data, not instructions: segment 40's residue is runs of
literal `0F 0F 0F 0F ...`, a table embedded in a code segment. Linear sweep
cannot tell that from code, and does not need to - lifting will use the call
graph rather than a sweep.

### The lift driver

`lift_ir32.py` is the NE front end these tools did not have - `decode16` and
`lift16` had no NE driver, and `generate.py`/`translator.py` are both 32-bit PE.
It feeds a segment through decode -> `lift16.Lifter.lift_function` -> C.

```bash
py tools/indeo/lift_ir32.py <IR32.DLL> --seg 1 -o ir32_seg1.c
py tools/indeo/lift_ir32.py <IR32.DLL> --seg 1 --stats
```

Segment 1 lifts to **53 functions, 4,504 lines of C**, 15 unhandled
instructions. So the pipeline works end to end.

**Finding function boundaries is the open problem**, and it has an NE-specific
cause. Every internal relocation in this DLL is a **SELECTOR** fixup: it patches
only the segment half of a far pointer, so `target_off` is *not* an entry point.
Reading it as one finds a single "entry" for a 20 KB segment.

The offsets are immediates in the calling instruction instead - 92 of the 112
internal relocations sit exactly three bytes past a `9A` (far call direct)
opcode, so the entry point is the `off16` just before the fixup. Recovering
them that way yields entry points for 35 segments and gives segment 1 its 53
functions with believable sizes (611, 217, 198, 181, 163; median 42).

### Classify before lifting

A late correction to the plan above. Segments 3 and 40 were called "the decode
core - leaf compute, the shape of pixel inner loops" because they are large,
make no calls and carry almost no relocations. Half of that was wrong.

`ir32_scan.py --classify` weighs three signals per segment:

| seg | bytes | entropy | `ret` | prologues | verdict |
|-----|-------|---------|-------|-----------|---------|
| 1 | 8,571 | 7.15 | 103 | 58 | code |
| 3 | 20,851 | 6.03 | 95 | 0 | code, frameless |
| **40** | **17,952** | **3.83** | **0** | 0 | **DATA - do not lift** |

**Segment 40 is a lookup table, not code.** 17,952 bytes holding 42 distinct
values in the range 0..187, no `ret` anywhere, and 75.8% of bytes equal to the
byte two positions later. NE marks it CODE because that is where the linker put
it; lifting it would have produced ~10,000 lines of plausible nonsense. It is
also where the earlier `0F 0F 0F 0F` decode "failures" came from.

That is good news twice over: a third of the supposed decode core does not need
lifting at all, and an 18 KB table of small values is very likely the VQ and
reconstruction tables our own decoder needs anyway.

**Segment 3 is real code but frameless** - 95 returns and not one
`push bp; mov bp,sp`. Optimised codec inner loops do not keep a frame pointer,
which is why prologue scanning finds no function boundaries in it.

Two things this does not yet solve:

### Carving functions by control flow

Frameless code has no prologue to split on, so the driver now finds function
extents by **recursive descent**: walk from an entry, branch at conditionals,
stop at a return or an indirect transfer, and treat near-call targets as new
functions. Segment 3 goes from one 6,900-instruction blob to real functions.

It also comes with a number that is worth being careful about. Descent from the
entry points we can actually find reaches:

| segment | reachable | functions |
|---------|-----------|-----------|
| 5 | 82.1% | 6 |
| 1 | 51.3% | 40 |
| **3 (decode core)** | **5.8%** | **3** |

Seeding fresh roots into the leftovers reports 100% coverage and 145 functions
for segment 3. That number is a lie. A code segment holds tables too, and
disassembling those manufactures very convincing functions - a run of `00`
bytes becomes `add [bx+si], al`, and one of the invented functions contains an
`int 0x0` in the middle of what is supposed to be a video decoder. Segment 3
has 4,846 zero bytes, which is where 142 imaginary functions came from.

So seeding is opt-in (`--seed-unreached`) and the default reports what it could
not reach instead of papering over it.

### Segment 3 dispatches through jump tables

The missing entry points are not an external dispatch table - they are internal.
Segment 3 contains **36 indirect jumps**, and the tables they read sit in the
code segment itself:

```
@0x0DB0  jmp word cs:[bx+si+0xDB8]
   table@0x0DB8:  0DF0 0000 0E68 0000 1BB0 0000 ...
```

Entries alternate with a zero word, and the non-zero values are valid
instruction addresses. This is one switch-dispatched decoder, which is exactly
the shape an Indeo cell decoder should have - a jump on the cell opcode into
per-case handlers.

Resolving them **during** descent changes nothing, and the reason is worth
recording: the tables live inside the functions that cannot be reached, so
control flow never arrives to read them. They have to be harvested statically,
by scanning every `jmp [mem]` with a constant displacement across the whole
segment and taking whatever its table points at.

That recovers 26 targets and takes segment 3 from **5.8% to 19.3%** reachable,
3 functions to 17.

The remaining 80% is still out of reach. Ten of the 36 indirect jumps take
their table address from a register (`jmp word cs:[si]`), so the table cannot be
found without tracking what `si` held - that needs light dataflow, not more
pattern matching. That is the next step.

**5.8% was the state before jump tables; 19.3% is the state now.** Segment 3 is genuinely code -
95 returns, entropy 6.03 - so the missing 94% is not table; it is functions
whose entry points we have not found. They are not near calls, not far calls,
not entry-table exports and not relocation targets, which leaves indirect calls
through a function-pointer table. Finding that table is the next step, and it
is now a single well-defined question rather than a vague one.

- **Segment 3 previously lifted as one 6,900-instruction blob.** Its entry points are
  not direct far calls: the callers build a far pointer from an immediate pair
  (`mov [bp-1A], off` then `mov [bp-18], seg`, the fixup landing on the segment
  half), which the driver now reads - but that yields only a couple of entries.
  Being frameless, it needs call-graph-driven boundaries rather than prologue
  or relocation scanning.
- **Data inside code segments gets lifted as code.** Segment 1's output
  contains a far call to `0000:FFFF` and an `int 0x21` - a DOS interrupt in a
  Windows DLL, which is a table being disassembled, not a function. The
  boundary pass needs to be call-graph driven rather than "split the linear
  sweep at known starts".

### After that



Deriving a whole codec from its output is slow, and this one has a shortcut
that fits what this project already does well. `IR32.DLL` on CD1 **is the
decoder** - it is just a 16-bit NE binary, so it cannot be loaded in-process
the way `DECO_32.DLL` was.

But it can be *lifted*. pcrecomp carries 16-bit tooling (`tools/lift/lift16.py`,
`tools/disasm/decode16.py`, `runtime/recomp16/`), and at 151 KB `IR32.DLL` is
the same order of size as `DECO_32.DLL` (134 KB), which was recompiled to
byte-exact C. That is the proven playbook here: lift it, run it, and use it
both as the working decoder and as the oracle that validates a clean-room one.

It also keeps the licensing clean - the result derives from a binary you own,
not from anyone else's source.

## Usage

```bash
indeodec -i <file.avi>            # stream info + frame headers
indeodec -c <file.avi>            # validate every frame against the model
indeodec -x <file.avi> -o <dir>   # extract frame payloads

# reference frames to check against, ffmpeg as an external oracle:
ffmpeg -i clip.avi -pix_fmt yuv410p -f rawvideo ref.yuv
```

`-c` is the regression test: run it over the whole collection and every clip
should report `OK`. A clip that reports no video chunks fails rather than
passing vacuously.
