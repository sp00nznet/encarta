# ftcdecode — clean-room FTC / FTT / FIF image decoder

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


## Status

The FTC colour path here is **superseded** by the statically recompiled
DECO_32 codec in [`tools/recomp`](../recomp/README.md), which is byte-exact
against the original DLL. This decoder is kept because it is clean-room work
that documents the format independently, and because FTT and FIF decode
perfectly through it.

See [docs/FORMATS.md](../../docs/FORMATS.md) for the format itself.
