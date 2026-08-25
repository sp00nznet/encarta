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

**Segments 2 and 3, about 23 KB, are the 32-bit decode core.** The rest are
16-bit driver scaffolding: `DriverProc`, the VFW `ICM_*` message handlers, the
dialogs.

Segment 13 also scores 32-bit here, and should be read with suspicion: 0.002
against 0.001 is not a verdict, it is a coin landing on edge. It is not code at
all - see the zero-return check below - and a test that has to choose between
two widths will always return one of them for data.

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

### A code segment returns

Segment 13 carved into one function covering 100% of the segment, which reads
as the best row in the table. It is data.

The signal that gives it away is blunt: **not one `ret` in 3,071 bytes**. Code
returns. Optimised code, frameless code, hand-written assembly - all of it
returns. A blob of data, meanwhile, carves into exactly one enormous "function"
spanning the whole segment, because descent never meets a return to stop at, and
that is indistinguishable from success if you only read the coverage column.

The lift confirmed it after the fact. Segment 13's unhandled instructions were
`daa`, `das`, `aaa`, `aas`, `bound`, `arpl`, `lldt`, `ltr`, `insb`, `outsb`,
`packuswb` - BCD, string and system opcodes that no video codec emits. The
carve-quality check missed it because at 65 junk instructions in 1,385 the
density is 4.7%, just under its 5% threshold. The zero-return test does not care
about thresholds.

`--stats` now warns on it. Across the file it fires on segments 13, 39 and 40 -
exactly the three that other evidence already called data.

### The driver loads

```
DRV_LOAD    -> 00000001
DRV_ENABLE  -> 00000001
DRV_OPEN    -> 00003FB8      <- a live driver instance
imports reached (8 distinct): KERNEL 132, 197, 16, 15, 18; GDI 52, 80, 68
```

Three more things had to be right, and each was a small correction rather than
new machinery.

**The jump table in a 16-bit segment is AT the displacement.** The two halves
are opposites here. 16-bit code addresses its table directly - `jmp word
cs:[bx+0x4A]` means the table is at 0x4A, BX already scaled - while the 32-bit
core's displacement is a base the loader fixes up and the table sits just past
the jump. Applying either rule to the other half finds nothing, and DriverProc
dispatches its messages through exactly such a table.

**lift16 emitted a comment where a dispatch belonged.** Its indirect-jump
handling is off by default, so `jmp word cs:[bx+0x4A]` became
`/* needs dispatch */` and execution fell through the switch into whatever
followed. That is why the first driver call ended up somewhere no near call
could explain.

**`tail-jmp 0xNNNNNN` is base + target too**, exactly like `res_<n>`:
0x017D + 0x04B6 = 0x000633. It only looked right in DriverProc, whose base is
0. Reading the instruction's own trailing comment instead took segment 6 from
42 stubbed targets to one, and the whole DLL to zero.

### DPMI, and a fact arriving from the other direction

Running DRV_LOAD reaches `int 31h`, and what it does there is worth recording:

```
mov bx, [bp+6]        ; a selector
lea di, [bp-8]        ; an 8-byte descriptor buffer
mov es, ax            ; ES:DI = buffer
mov ax, 0x000B        ; DPMI get descriptor
int 31h
or byte [bp-2], 0x40  ; byte 6, bit 6 - the D/B bit
mov ax, 0x000C        ; DPMI set descriptor
int 31h
```

The driver is **marking its own segments 32-bit at load time**. That is the
same fact the lift had to establish by decoding every segment both ways and
comparing junk rates, arriving from the opposite direction - and it explains
why nothing in the file records the width statically. There is nothing to
record. The DLL sets it itself, at run time, which is why NE flag 0x1000 was
never going to be the answer.

The runtime implements enough DPMI for that (get/set descriptor, allocate
descriptors, the base and limit calls accepted and ignored, since `g_segoff`
already says where a segment is). Setting the D/B bit changes no behaviour
here; refusing the call would stop the driver dead.

### The codec accepts the format

```
DRV_LOAD               -> 00000001
DRV_ENABLE             -> 00000001
DRV_OPEN               -> 00003FCA
ICM_DECOMPRESS_QUERY   -> 00000000     ICERR_OK
ICM_DECOMPRESS_BEGIN   -> 00000000     ICERR_OK
ICM_DECOMPRESS         -> FFFFFFFF     ICERR_UNSUPPORTED
output: 0 of 124416 bytes non-zero (0.0%)
crossings into the 32-bit core: 7
```

The frame is real - `T010532A.AVI` from the disc, 216x192 IV32, first frame a
15,844-byte keyframe - and **the codec now agrees to decode it**. `QUERY` and
`BEGIN` both return `ICERR_OK`, which means the driver has looked at the input
and output headers and accepted both.

**Seven crossings into the 32-bit core.** The decode path reaches the lifted
32-bit Indeo code and returns from it. Both halves execute, the bridge between
them carries the call, and the failure that remains is the driver's own answer
rather than the recompilation falling over.

That counter exists because an empty output buffer looks the same whether the
decoder ran and wrote nothing or was never called at all, and those need
completely different fixes.

### Three things stood between "returns garbage" and "returns ICERR_OK"

**A fallthrough is not a far call.** A function that runs into the next one
emits:

```c
recomp_dispatch(cpu, 0x52, 0xE); return;   /* fallthrough 0x00052E */
```

There is no call here - control simply continues at the next function - and the
address is split as a real-mode paragraph *and* carries the lifter's base, so
the true target is `0x00052E - 0x0280 = 0x02AE`. Every ICM message handler ends
this way, so before the fix every message returned without doing anything. 33
sites across two segments.

**The harness pushed the wrong return address.** `ir32_driver_call` pushed zero,
so DriverProc's own `retf` dispatched to `0000:0000` and read as a missing
function. 0xFFFF is lift16's marker for a return address it does not know, and
the runtime already treats it as the end of a call.

**A codec is opened through ICM, not just DRV_OPEN.** `lParam2` carries an
`ICOPEN` saying what the driver is being opened *for* - `fccType` 'vidc',
`fccHandler` 'IV32', `dwFlags` ICMODE_DECOMPRESS. Without it the driver has no
reason to believe it is being asked to decompress anything.

### The local heap was missing, and that was the last of it

`DRV_OPEN` calls `LocalAlloc` three times, and LocalAlloc hands out **near**
offsets into the automatic data segment - so it needs heap room inside that
segment, which the loader was not providing. The NE header asks for
`ne_heap = 1024` bytes.

1024 is not enough: the driver asks for 1116 in a single call. That is not a
contradiction, it is what `ne_heap` means - the *initial* heap, which Windows
grows on demand within the 64K a segment can address. Giving the data segment
the rest of its 64K, which is what a real DGROUP has, took `QUERY` and `BEGIN`
from `ICERR_BADPARAM` to `ICERR_OK`.

### Identifying the two imports without an ordinal table

There is no Win16 KERNEL on the disc to read an export table from, so the
ordinals cannot be looked up. They can be read off their call sites, which is
better evidence anyway - a name would still have to be checked against what the
code does with it.

The ordinals already confirmed behaviourally (5 LocalAlloc, 15 GlobalAlloc, 16
GlobalReAlloc, 18 GlobalLock) all agree with the standard table, so the table
applies; only these two were unclear.

**KERNEL.171 is an alias.** It is handed the selector `GlobalLock` returned one
line earlier - stored at `es:[bx+0x3038]` - and its result is stored
immediately after it at `es:[bx+0x303A]`. A second selector kept beside the
first and made from it is an alias, and `AllocSelector`, `AllocCStoDSAlias` and
`AllocDStoCSAlias` all have the same observable effect: another selector over
the same bytes. Which of the three it is does not change what the runtime must
return, so it is implemented to the contract rather than to a guessed name.

**KERNEL.188 converts a count into a byte size.** It takes one word - always 3
here - and its DWORD result goes straight into `GlobalAlloc` as `dwBytes` and
is then decremented and sign-tested as a loop bound. Three segments' worth is
`3 << 16`. If that reading is wrong the buffer is the wrong size, which shows
up as a decode writing the wrong amount rather than as silence.

### The import convention was wrong, and it was hiding

Win16 is Pascal: **the callee pops its arguments.** The runtime was popping only
the far return address, so every import left its arguments on the stack and the
*next* call read its parameters from the wrong slots. That is how `GlobalAlloc`
came to be called with a size nothing had given it - the two-byte argument to
the call before it was still sitting there.

There is now an argument-size table, and an ordinal that is not in it says so
rather than assuming zero. KERNEL.132 and .197 are still unknown and still
announce themselves.

### Where the decode actually gets to

`IR32_TRACE=1` prints every crossing into the 32-bit core, which says far more
than the return code:

```
ICM_DECOMPRESS_QUERY  ->  3:0360                     -> ICERR_OK
ICM_DECOMPRESS_BEGIN  ->  3:0000, 3:0000             -> ICERR_OK
ICM_DECOMPRESS        ->  3:0505, 3:0610 x3          -> ICERR_UNSUPPORTED
```

3:0610 is **the decode entry** - the 28-argument function whose first act is to
dereference a far pointer for the instance's data selector - and it is called
three times, which is what a three-plane YUV decode should look like. 3:0000 is
the initialisation the `init` command already checks against the disassembly.

So the driver reaches its own decoder. All three calls then return identical
registers, with `eax` holding 0x0302 - the selector of the *input* buffer we
passed in - so it is returning early rather than decoding, and the 16-bit half
turns that into ICERR_UNSUPPORTED.

### It does not decode - a retraction

The previous version of this file said "the lifted codec decodes the frame",
on the strength of this:

```
selector 0403: 56667 of 65536 bytes non-zero
selector 0404: 56667 of 65536 bytes non-zero
```

That was wrong. Those buffers begin `66 33 C0 B8 CC CC CC CC 8B D5 33 ED` -
segment 3's thunk prologue - and they are **verbatim copies of the codec's own
code**, 20,841 of 20,851 bytes identical to segment 3, the difference being the
ten bytes the loader patches. A 16:32 codec copies its 32-bit code into a
selector it can execute, and that is what those non-zero bytes are.

The conclusion was drawn from byte counts without checking what the bytes were.
It is the exact failure this file keeps warning about, arrived at by the exact
route it warns against, and the check that would have caught it immediately -
compare against a decoder known to be right - was the one thing not done.

### The check, now that it exists

`verify_decode.py` compares the dumped buffers against ffmpeg's decode of the
same frame. Nothing about the codec's internal layout is assumed: a correctly
decoded plane has to contain ffmpeg's rows verbatim somewhere, so finding one
fixes the offset and the stride follows from the next.

```
ffmpeg -i T010532A.AVI -frames:v 1 -pix_fmt yuv410p -f rawvideo ref.yuv
IR32_DUMP=buf ir32_run IR32.DLL decode frame0.bin 216 192 nul 8
py verify_decode.py buf ref.yuv 216 192
```

No exact row appears in any buffer. The nearest approach is a mean absolute
difference of 6.31 on a chroma row, which the tool reports next to the
buffer's own distribution precisely so it is not mistaken for a hit: chroma
rows are near-constant, the buffer has runs of near-constant `7D`, and the
rows after it score 44 at the best stride. There is no plane structure. The
frame is not decoded, in any layout, anywhere in those buffers.

### The codec calls a copy of itself

Tracing every far call in the 16-bit half - `IR32_TRACE16=1` - showed the whole
`ICM_DECOMPRESS` path in one run:

```
6:0000 -> 1:01DE -> 6:02EC -> 7:0714 -> 5:06C8
   -> 3:0505, 3:0610 x3
   -> 0404:2C10        no lifted entry
```

Selector 0x0404 is one of the driver's own `GlobalAlloc` blocks, and 0x2C10 is
an entry in **segment 3**. A 16:32 codec does not call its 32-bit code where
the loader put it: it allocates a block, copies the code segment in, marks the
descriptor 32-bit through DPMI - which is the `int 31h` sequence documented
above - and calls the copy. So the call arrives as `0404:2C10` while every
lifted function is registered under segment 3.

`ne_code_alias` resolves it. A copy is byte-identical to its original bar the
handful the loader patches, so comparing 256 bytes identifies it, and the
answer is cached. The comparison is deliberately not `memcmp`: the loader
writes selectors into the original *after* the copy was taken, so a few bytes
legitimately differ.

That entry, 0x2C10, is one of the two thunks that only turned up by matching
the thunk *body* rather than its `0xCCCCCCCC` fixup placeholder. Had that
search been left at the placeholder, this call would have had nowhere to land
and the reason would have been considerably harder to see.

### Into the decoder, and out the other side

With the code copy resolved the run reaches 3:2C10 - the real decode thunk -
and **faults inside it**. That is progress: the -1 the driver appeared to be
returning was the harness's own fault handler, not the codec's answer.

Two things came out of chasing it.

**String operations were addressing memory with no segment base.** The lifter
emits a whole `rep movsd` as one statement built from ESI/EDI directly:

```c
while (c->ecx) { wr32(c->edi, rd32(c->esi)); ... }
    /* rep movsd dword ptr es:[edi], dword ptr fs:[esi] */
```

Correct for a flat image, wrong here - and overriding `rd`/`wr` does not reach
it, because the statement is built whole. The comment names both segments, so
the NE driver rewrites it from there rather than assuming the defaults. Two
forms occur in this DLL, `rep movsd` and `rep stosb`, and both were wrong.

**The fault is a far pointer being used as a flat offset.** `IR32_WATCH` records
every address touched, which a single fault address cannot substitute for:

```
20F9E0C4  in arena
20F9E0D4  in arena
20F9E0D4  in arena
24FF111C  OUTSIDE arena
```

The decoder runs normally inside the arena and then steps outside in one
instruction. The register behind it held `040611FC`, which is not a large
number - it is **selector 0x0406, offset 0x11FC**, and 0x0406 is one of the
driver's own GlobalAlloc'd buffers. A 16:16 far pointer is being consumed as a
32-bit offset.

That is the shape of the remaining problem, and it is a model question rather
than a bug to patch. Real 32-bit Indeo code runs with descriptors whose bases
make a linear address meaningful, so a pointer built in one place is usable in
another. This runtime gives every selector its own base into one arena, which
keeps faults loud and traces readable, and makes a linear address from the
DLL's own world meaningless. Whether that pointer should have been loaded with
`les` and lost its segment half somewhere, or whether the decoder genuinely
expects a flat view, is the next thing to establish - and it decides whether
the fix is one instruction or the memory model.

### What is actually established

| | |
|---|---|
| `init` | 3:0000 runs and its output matches the disassembly |
| `sweep` | 191 of 217 32-bit entries return |
| `driver` | DRV_LOAD, DRV_ENABLE, DRV_OPEN all succeed |
| input | reaches the codec intact - passes its own 'FRMH' checksum |
| output format | 8 and 16 bpp accepted; 24 and 32 refused |
| `decode` | reaches the decode thunk, runs inside it, faults on a far pointer |

The output buffer receives 8,984 bytes of the constant 4 across rows 143-191.
That is a background fill, not an image - two distinct values in the whole
buffer - and it happens before the fault.

### What the codec's working buffer looks like

Selector 0x0405 - the instance the decoder works in - is not empty and is not
noise:

```
byte histogram: 04 x37449, 00 x22407, 7E x770, 07 x642, 06 x596, 7B x572
runs of 0x04:  0x0088..0x0171 (233)   0x0188..0x0228 (160)
               0x0288..0x0371 (233)   0x0388..0x0428 (160)
```

Runs starting every 256 bytes, alternating 233 and 160 long, with other values
between them. That is image-shaped: a stride, a dominant flat value, and
detail around it. It is what a partially decoded plane would look like.

It still does not match ffmpeg. `verify_decode.py` finds no row of the
reference in any buffer, and the nearest approach is a chroma row at 6.31
against a 5th-percentile of 30 - the same near-constant coincidence documented
above, not a hit. So something decoder-shaped is happening and it is not
producing this frame.

The fill that reaches the output buffer is the same byte, 0x04, which is why
8,984 bytes of it arrive there and nothing else does.

### The bridge was clearing registers a far call preserves

The decode thunk's first three instructions are:

```
mov [0xe188], edx      ; saves the incoming EDX
mov [0xe190], edi      ; saves the incoming EDI
mov [0xe18c], esi      ; saves the incoming ESI
```

It saves them because they are inputs - a far call does not touch the general
registers, so whatever the 16-bit caller left is part of the call. `ne_call32`
started from a `memset` machine and handed the decoder three nulls instead.
Fixed: the caller's registers are carried across.

SI, DI and BP are 16-bit in pcrecomp's 16-bit CPU, so only their low halves
survive the trip. That is a limit of the model rather than a shortcut here, and
it is recorded because it will matter if a callee ever needs the full 32 bits.
This DLL's 16-bit half never uses the 32-bit forms - checked, not assumed - so
it is not the current problem.

### One working buffer is intact, the other is full of pixels

The two 136 KB buffers the driver allocates should be symmetric - current
frame and reference frame, both initialised by `3:0000` during BEGIN. After a
decode they are not:

```
sel 0406  [0x0C]=0000E2C0  [0xE1A8]=0000FB80  [0xE198]=00016A34  [0xE190]=0000FEC0
sel 0405  [0x0C]=04040404  [0xE1A8]=04040404  [0xE198]=04040404  [0xE190]=04040404
```

0x0406 holds coherent state: the plane table where `3:0000` put it and working
pointers that have advanced sensibly. 0x0405 reads `04040404` everywhere,
including the globals the decoder depends on. The fault follows directly: `EDI`
is loaded from `[0xE1A8]`, and `SEGB(ds) + 0x04040404` is the faulting address
to the byte - `arena + 0x0FFF20 + 0x040611FC`.

`0x04040404` is **not a fill**. It is what Indeo's own arithmetic produces:

```
add eax, 0x4040404        ; +4 to each of four packed pixels
and ecx, 0x78787878       ; the averaging mask, four 4-bit lanes
```

Five `add eax, 0x4040404` sites sit inside the decode thunk. So those bytes are
decoded pixels whose value happens to be 4 everywhere, written over the region
that holds the decoder's own state.

### One buffer proves the decoder works

The two working buffers, read after a decode:

```
0406   stride=000000A2  cursor=0000FB80  planes=00010734 / 00016A34 / 00018C94
0405   stride=04040404  cursor=04040404  planes=04040404 / 04040404 / 04040404
```

0x0406 is **coherent decoder state**: a plausible stride, a cursor that has
advanced well into the buffer, and three distinct plane pointers spaced like
Y/U/V. Nothing about it is accidental - a run that had gone wrong everywhere
would not leave one buffer looking like that. The lifted decoder does work.

0x0405 has `04040404` in every field, including the plane table - and that
table was verified correct immediately after BEGIN:

```
after BEGIN:      sel 0405 head: ... 0000E2C0 0000E318 00018C94 0000E2EC
after DECOMPRESS: sel 0405 head: 04040404 04040404 04040404 ...
```

So it is corrupted between those two points, and only three calls happen in
between: 3:0505 once and 3:0610 three times. That is the search space now.

### A 16-bit address does not sign-extend, it wraps

The fault is fixed, and the bug behind it is general.

A 0x67 prefix in a 32-bit segment makes the effective address 16-bit, and a
16-bit address **wraps inside the segment**. Capstone reports the displacement
signed, so

```
mov ebp, es:[0xE188]
```

arrives as `0xFFFFE188` and was emitted as `SEGB(c->es) + 0xFFFFE188u` - which
reads 0x1E78 bytes *below* the segment rather than at offset 0xE188. The
codec's own globals live at 0xE188, so every access to them was landing outside
its buffer, reading and writing whatever preceded it in the arena.

211 operands in the decode core carry that prefix. Masking the effective
address to 16 bits when it is present:

| | before | after |
|---|--------|-------|
| fault | reads 67 MB outside the arena | **none** |
| output written | 8,984 bytes (21.7%) | **27,080 bytes (65.3%)** |
| `ICM_DECOMPRESS` | harness fault handler | `ICERR_OK` |

The decode now runs to completion.

### Caught in the act

`IR32_WATCH` reports reads and writes inside a chosen window, with the value
written - because "something wrote here" cannot tell a pointer being stored
from pixel data landing on top of one. Pointed at the plane table:

```
-> 3:0000     WRITE32 +0000 = 00010734      the real table
              WRITE32 +0004 = 0000E2C0
-> 3:0505     (nothing)
-> 3:0610 x3  reads only
-> 3:2C10     WRITE32 +0000 = 04040404      <- packed pixels
```

That was the sign-extended address in action: writes meant for the globals at
0xE188 landing at the start of the buffer instead.

### Mutable variables inside the code segment

Also fixed on the way, and worth recording because it explains the code copy:

```
cmp eax, dword ptr cs:[0x2F7D]     ; read through CS
mov es, [ebp+4]
mov dword ptr es:[0x2F7D], eax     ; write through a data alias
```

The codec keeps mutable variables *inside its own code*, and a code segment
cannot be written - so it allocates a writable copy, marks it 32-bit through
DPMI, and writes there. CS must therefore stay the selector the caller used
rather than the segment the lifted functions are registered under, or the
decoder reads one copy and writes the other. `find()` resolves the alias for
dispatch while CS keeps the copy.

### Where this got to

**The recompiled decoder decodes.** Against ffmpeg's decode of the same
packet, the luma plane the lifted code produces is:

```
columns   0..167     100.0% byte-exact, all 192 rows
columns 168..175      99.8% wrong
overall (176 x 192)   95.5% byte-exact
```

`verify_plane.py` reproduces that from a dumped buffer. The comparison is
`ours * 2`: Indeo 3 works in six bits and ffmpeg scales by four on output,
while this plane holds twice the six-bit value.

So the bitstream reader, the codebooks, the reconstruction and the packed
arithmetic are all correct. What is left is one number - the decoder fills 42
dwords of each row where a 216-wide frame needs 54 - and a colour-conversion
step whose output pitch does not match the DIB.

#### What it took to see it

Two things hid a working decoder behind what looked like a broken one.

*The output never was the picture.* The six colour converters write at a
hardcoded 256-byte row stride (`[ebp+0x100]`, `[ebp+0x200]`, `[ebp+0x300]`)
while being handed a base computed at the DIB's 216-byte pitch. Whatever the
decode did, what came back through ICM_DECOMPRESS could not be a picture. And
at 8bpp those bytes are palette indices, so comparing them to luma reports
noise however right the decode is.

*Half of every buffer was invisible.* The codec's working buffers are
GlobalAlloc'd and most are bigger than a segment - 0405 and 0406 are 137,024
bytes each - but the dump wrote a hardcoded 65,536 and the report said "of
65536 bytes" regardless. The plane is at 0x10734 and runs to 0x18B34, which
are exactly the codec's own [E1AC] and [E198]. Every earlier conclusion that
"the plane buffers do not correlate" was drawn from the first half of them.

The lesson worth keeping: `verify_plane.py` locates the plane by
**correlation** and only then checks it byte-exactly. Byte matching alone
finds nothing when the values are on a different scale, and answers "not
decoded" - which is what this file said for a long time while the picture sat
in the buffer.

#### The bug that got it here

105 of segment 3's 221 functions were truncated. carve_functions ends a
function where the next entry begins, so a function whose body continues into
the next one was emitted with no final control transfer - a silent `return`
mid-body. The per-plane worker at 3:0610 stopped at `mov ebx, 0x80`, which is
why the bitstream reader consumed 16 bytes of a 15,844-byte frame. It now
consumes 15,840.

#### Checked and clean

Both lifters were differential-tested against Unicorn's x86: every distinct
instruction run twice on identical state, registers, flags and memory
compared. Across 37 segments and ~7,000 instructions the only disagreements
are shifts not setting OF, mul/imul writing flags x86 leaves undefined, and
16-bit adc/sbb losing CF when `src+cf` wraps. None is read on this path.

`KERNEL.197` was popping nothing where one word is pushed, skewing the
caller's stack through driver initialisation. Fixed; it did not change the
output.

Still open:

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
