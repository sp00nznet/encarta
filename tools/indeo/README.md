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

### A better route than inference

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
