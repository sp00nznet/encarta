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
| 3 | 20,851 | 6.03 | 95 | 0 | code |
| **40** | **17,952** | **3.83** | **0** | 0 | **DATA - do not lift** |

**Segment 40 is a lookup table, not code.** 17,952 bytes holding 42 distinct
values in the range 0..187, no `ret` anywhere, and 75.8% of bytes equal to the
byte two positions later. NE marks it CODE because that is where the linker put
it; lifting it would have produced ~10,000 lines of plausible nonsense. It is
also where the earlier `0F 0F 0F 0F` decode "failures" came from.

That is good news twice over: a third of the supposed decode core does not need
lifting at all, and an 18 KB table of small values is very likely the VQ and
reconstruction tables our own decoder needs anyway.

### The decode core is 32-bit, and nothing in the file says so

The name is literal. `IR32.DLL` is an NE - a 16-bit container - but the codec
inside it is 32-bit, and the segment table does not record which segments are
which. There is no USE32 bit to read. NE flag `0x1000` is **DISCARDABLE**, not
32-bit; every segment in this DLL sets it, including ones that are certainly
16-bit.

So bitness has to be measured. `segment_bitness()` decodes each segment both
ways and counts the tells - bytes that decode to nothing, and instructions whose
bytes are all zero, which is what the top half of a 32-bit operand looks like
when something reads it as 16-bit:

| seg | as 16-bit (junk rate) | as 32-bit (junk rate) | verdict |
|-----|----------------------|-----------------------|---------|
| 1 | 0.016 | 0.027 | 16-bit |
| **2** | 0.057 | **0.000** | **32-bit** |
| **3** | 0.176 | **0.062** | **32-bit** |
| **13** | 0.002 | **0.001** | **32-bit** |
| 5 | 0.000 | 0.021 | 16-bit |

**Three segments - 2, 3 and 13, about 26 KB - are the 32-bit decode core.** The
other 37 are 16-bit driver scaffolding: `DriverProc`, the VFW `ICM_*` message
handlers, the dialogs.

Segment 1 is the useful counter-example. It sets exactly the same flags as
segment 3, so a flag-based reading calls both 32-bit, but its own code settles
it - `8B 46 06` is `mov ax, [bp+6]`, the standard 16-bit far-call argument
fetch, and it ends `lea sp, [bp-2]` / `pop ds` / `retf 4`.

### What reading it at the wrong width did to the analysis

Decoding 32-bit code as 16-bit does not fail. It resyncs, and produces
confident nonsense. `mov eax, [edi+0x20c]` (`89 87 0C 02 00 00`) splits into
`mov ax, [bx+0x20c]` plus `add [bx+si], al` on the abandoned high half. Read
correctly, segment 3 at 0x0F24 is an ordinary plane copy:

```
0x0F3D  8B 87 50 FF FF FF   mov eax, dword ptr [edi - 0xb0]
0x0F45  89 07               mov dword ptr [edi], eax
0x0F47  89 87 B0 00 00 00   mov dword ptr [edi + 0xb0], eax
0x0F4D  8D 7F 04            lea edi, [edi + 4]
```

Three earlier conclusions in this file were artifacts of the wrong width, and
are retracted:

- **"Segment 3 is frameless - 95 returns and not one `push bp; mov bp,sp`."**
  It has neither the returns nor the prologues attributed to it; those counts
  came from a misaligned sweep.
- **"Segment 3 has 4,846 zero bytes"** - 23% of the segment, against ~5%
  everywhere else. Those are not padding or tables. They are the high halves of
  32-bit displacements and immediates. Read as 32-bit the anomaly disappears.
- **"Segment 3 dispatches through jump tables whose entries alternate with a
  zero word."** The tables are real, but the entries are 4 bytes, not 2 - the
  "alternating zero word" was the top half of each. The reported gain from
  harvesting them (5.8% -> 19.3% reachable) was measured on a mis-decoded
  segment and does not carry over.

The decode-coverage table further up has the same defect for its seg 2 and seg 3
rows, which were measured 16-bit. The FS/GS prefix fix it describes is still
real and still needed - there are 37 genuinely 16-bit segments here - but those
two rows do not mean what they say.

### Finding entry points

Reading the width correctly fixes the disassembly but not the boundaries. A
segment is entered by FAR call from other segments, so scanning one segment for
near calls finds almost nothing - segment 1 yielded zero. Four sources, in order
of yield:

**Far calls, via relocation chains.** The pair that names a callee is split
across two places: a SELECTOR relocation names the target *segment*, and the
call instruction's own immediate holds the *offset*.

```
9A 64 04 FF FF      call far <seg 9>:0x0464
```

Two details matter. `FF FF` is an end-of-chain marker, not a selector - NE
chains several fixup sites per relocation record through the patched words, so
following the chain finds **334 sites behind 112 records**. And a SELECTOR fixup
is not always a call: `B8`/`B9`/`BB imm16` is `mov reg, <selector>`, an ordinary
data-segment load. Only sites preceded by `9A` are entry points. That yields
**285 entry points across 34 segments**.

**The entry table.** Six exported ordinals - `DriverProc`, `LibMain`, `WEP`,
`DriverDialogProc`, `AboutDialogProc`, `___ExportedStub`. Small, but they are
the roots the driver is actually called through.

**Prologues.** `55 8B EC` at an address the sweep already decodes as an
instruction. Worth nothing in the 32-bit core, which keeps no frame pointer, but
segment 1 has 50 of them and that is most of its coverage.

**Descent, bounded.** Walk from each entry, branch at conditionals, stop at a
return, at padding, or at another known entry. The last of those matters:
without it a walk runs off the end of one function into the next, and 11
"functions" in segment 3 all reported ~1,300 instructions because they had
swallowed their neighbours. Padding is zero fill or MSVC's `int3`.

### Where it stands

| segment | bits | reachable | functions |
|---------|------|-----------|-----------|
| 1 | 16 | 68.2% | 84 |
| 2 | 32 | 100% | 2 |
| **3 (decode core)** | **32** | **33.4%** | **20** |
| 5 | 16 | 98.7% | 7 |
| 13 | 32 | 100% | 1 |

**32 of the 40 code segments now reach 90% or better.** The 16-bit driver half
is essentially solved. What is left is the part that matters: segment 3, the
largest piece of the 32-bit core, still reaches only a third. It has no
prologues to seed from and only 4 far-call entries, so its interior is reached
by indirect call through a table this pass has not found yet.

`--stats` reports a carve-quality check alongside the coverage number, because
recovering a fake entry point is not a loud failure - the lifter emits confident
C for it just the same. It flags any function whose body is more than 5%
zero-fill or junk opcodes. That check is what caught the mis-decoded segments in
the first place: 10 of 16 segment-3 "functions" were flagged before the width
was corrected, and none are now.

### Three ways a linear sweep lies

Coverage sat at a third of the decode core for a while, and every remaining
obstacle turned out to be the same mistake in a different costume: trusting the
linear sweep to say where instructions begin. A sweep is misaligned wherever it
crosses a table, and it stays misaligned until it happens to resync.

**The jump table is not at the displacement.** Segment 3 has 34 indirect jumps
and no indirect calls at all. Twenty-six of them look like this:

```
0x1C32   jmp dword ptr cs:[edx*4 + 0x185C]
```

Read 0x185C as the table and you get garbage, which is why they were written off
as needing dataflow. The table is at **0x1C3C** - eight bytes of instruction,
two bytes of `mov eax, eax` alignment padding, then the entries. The
displacement is a base the loader fixes up and means nothing statically. Looking
just past each jump instead resolves **32 of 34 tables, 103 targets**.

**A target need not be in the sweep.** Validating those entries by membership in
the linear sweep truncated the tables - the one at 0x1C3C appeared to start at
0x1C44, losing its first two entries, because the sweep was misaligned across
exactly that region. Decoding *at* the candidate address instead settles it
honestly.

**A root that is not in the sweep was silently dropped.** This was the
expensive one. `carve_functions` skipped any root the sweep did not already list
as an instruction boundary, so entries recovered at real cost - thunk prologues,
jump-table targets - were discarded without a word. Seeding 0x2800 did nothing
whatsoever. Re-decoding forward from the entry and splicing the result into the
sweep, stopping when it rejoins, took segment 3 from 46% to 81% on its own.

### The 32-bit segments have their own entry signature

A 32-bit function callable from this DLL's 16-bit half opens with a thunk that
converts the caller's frame and loads GS with the bitstream selector:

```
66 33 C0            xor ax, ax
B8 CC CC CC CC      mov eax, <load-time fixup placeholder>
8C DB / 8E C3       mov ebx, ds / mov es, ebx
8B D5 / 33 ED       mov edx, ebp / xor ebp, ebp
66 8B EC            mov bp, sp
66 8E 6D 08         mov gs, word ptr [ebp + 8]
```

Match the **body**, not the placeholder. Searching for `B8 CC CC CC CC` finds
nine entries, four of which were already known - and misses two that are
identical except for the placeholder, `mov eax, 0xF8F70000` at 0x2C10 and
0x3640. Those two turned out to be the entries to the last two unreached code
runs, worth 11 points of coverage between them. Neither pattern is a superset of
the other, so the driver takes both.

Reaching that code also settled what it is. The two runs were briefly written
off as data on the strength of their first 56 bytes, which really are a table -
each function is followed by one, a 16-byte header and then 8-byte records
pointing back into the function itself. Measuring pointer density across the
whole run rather than eyeballing its head showed only 14% of dwords were
plausible addresses, so the bulk was something else. It is the pixel
interpolation:

```
0x3B07   xor ecx, edi
0x3B09   and ecx, 0x78787878     <- four 4-bit lanes packed in a dword
0x3B0F   add ecx, edi
```

### Where it stands

| segment | bits | bytes | covered | functions | lines | unhandled |
|---------|------|-------|---------|-----------|-------|-----------|
| 1 | 16 | 8,571 | 77.2% | 97 | 3,856 | 3 |
| 2 | 32 | 2,222 | **100%** | 2 | 870 | 11 |
| **3 (decode core)** | **32** | **20,851** | **92.5%** | **108** | **12,539** | **130** |
| 5 | 16 | 6,920 | 99.6% | 7 | 2,541 | 1 |
| 13 | 32 | 3,071 | **100%** | 1 | 1,414 | 65 |

Coverage is reported in **bytes**, not instructions. Resync adds instructions
the sweep never had, so "instructions reached / instructions swept" compares two
different denominators and drifts upward on its own. Bytes cannot.

The largest unreached runs in segment 3 are now the pointer tables themselves -
304 bytes at 0x4E10 and 195 at 0x3BFD, both 100% dwords that are zero or a valid
address - which is exactly what should not be lifted. `--stats` reports that
density per run rather than a verdict, because two earlier attempts to classify
code against data automatically, one by entropy and one by counting small
dwords, each got a known case backwards.

The unhandled counts are real and were briefly reported as zero. The 16-bit
lifter marks a gap with the token `UNHANDLED`; `lift32_cpu` marks it
`/* TODO */ abort();`, and the driver was only counting the former, so the
32-bit output looked perfect while carrying 130 gaps. What it is missing in
segment 3: `rol` (24), `lds` (22), `shld` (12), `retf` (12), `les` (2). The far
pointer loads are the interesting ones - `lds`/`les` are what segmented 32-bit
code does and a flat PE never needed. The remainder - 16 `int1`, 7 `aaa`, 4
`aas`, 3 `sldt` - are not real instructions but the residue of a few carved
regions that are still misaligned.

### Lifting

32-bit segments emit through pcrecomp's `lift32_cpu` - the same lifter that
produced the Encarta executable - rather than `lift16`. It is written against a
PE but only loosely: `read_va` is injectable and the image bounds are plain
attributes, so a 64K NE segment can pose as a tiny image based at 0. Three
things had to be fixed, each silent rather than loud:

- **Segment registers.** 23 functions failed outright on `mov ds, ax` - a flat
  PE never touches DS, so `lift32_cpu` had no case for it. They are now stored
  on the CPU struct, and `cpu.h` records the limit: stored, not used in address
  computation. Code that switches DS to reach another segment's data needs a
  selector-to-base mapping the runtime does not have.
- **Displacements were being GVA-wrapped.** The base class decides that from the
  PE `.reloc` table and falls back to a range check, which caught every small
  constant here - `lea edi, [edi+4]` became `edi + GVA(4)`. An NE segment is its
  own 0..64K space, so the NE driver overrides that to false.
- **Functions were named from the wrong address.** Naming and slicing from the
  lowest address descent reached, rather than the entry, emitted one shared
  region 16 times under the same name. Descent follows backward jumps, so
  `body[0]` is not the entry.

The emitted C is a faithful translation - the plane copy at 0x0F3D reads as it
should:

```c
c->eax = rd32((c->edi + 0xFFFFFF50u));   /* mov eax, [edi - 0xb0] */
wr32((c->edi), c->eax);                  /* mov [edi], eax        */
wr32((c->edi + 0x000000B0u), c->eax);    /* mov [edi + 0xb0], eax */
c->edi = (c->edi + 0x00000004u);         /* lea edi, [edi + 4]    */
```

It does not compile yet: there is no NE 32-bit runtime, and `GVA()` has to be
the identity there because an NE jump table holds segment offsets rather than
virtual addresses. The generated file says so in its own header.

Still open:

- **The last fifth of segment 3.** What remains is reached through a dispatch
  table the DLL builds at init - the thunk at 0x2800 reads `[eax*4 + 8]` in the
  data segment after loading GS with the bitstream selector. That table is
  written by code, not present in the image, so finding it statically means
  following what segment 3's own initialisation stores.
- **No NE 32-bit runtime.** Segment registers, `GVA()`, and far calls between
  segments all need one before any of this executes.
- **Data inside code segments still lifts as code** where descent reaches it.
  Segments 39 and 40 are excluded by classification; smaller tables are not.

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
