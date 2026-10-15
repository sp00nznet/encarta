# Contributors

The canonical record of who did what. The CHANGELOG tells the story change by
change; credit lives here. If you have contributed and are not listed, or a line
is wrong, open a PR against this file.

---

## Maintainer

### [@sp00nznet](https://github.com/sp00nznet)
Project creator and maintainer: the format work, the x86 lifter, the hybrid
harness, the recompiled Indeo codec, and the local-content runner.

---

## Issue reports and testing

### David Barnhart ([@davidbarnhart](https://github.com/davidbarnhart))
**Solved the topic-text encoding** (#2). Writing his own read-only Encarta 97
reader, he independently confirmed the `|Phrases` container model in
`docs/FORMATS.md`, then cracked what it listed as unsolved: phrase indices
above 0x9F are not bigger single-byte indices but a **two-byte reference**
(`0xA0-0xAF` = `phrase[(c & 0x0F) * 256 + next]`, `0xB0-0xBF` the same plus a
trailing space), and he described the `0x01` literal escape and the text record
markup. He also suggested anchoring each topic's LZ77 stream on its title from
`|TTLBTREE` rather than brute-forcing the start, which decodes all 31,109 topics
in about 13 s on his reader. `tools/mvbtext` implements the encoding (86b3a41);
the anchoring is the next task in [ROADMAP.md](ROADMAP.md).

---

## Borrowed from sibling projects

- **`cmake/clang-cl-x86.cmake`** - the cross toolchain that builds the 32-bit
  Windows harness on Linux and macOS - comes from
  [redalert2-recomp](https://github.com/sp00nznet/redalert2-recomp), where
  [@cpressland](https://github.com/cpressland) contributed it in #1.

---

A note on AI-assisted contributions: they are welcome, provided a human
understood and verified the change.
