# Encarta 97 file formats

Reference documentation for the container and data formats in Encarta 97,
written from the on-disk bytes and verified against the shipping files. Field
offsets are decimal unless prefixed `0x`; everything is little-endian.

The formats here are not Encarta-specific. `.M20` is Microsoft **Multimedia
Viewer 2.0**, a superset of WinHelp 3.1, and the same layout carries other
Viewer titles (Encarta 95/96, Cinemania, Bookshelf, Musical Instruments). The
image codec is Iterated Systems' fractal codec, licensed into the product.

| Format | What | Status |
|---|---|---|
| [M20 / MVB container](#m20--mvb-20-container) | the archive everything lives in | **complete** |
| [`\|Phrases`](#phrases-the-phrase-dictionary) | phrase-compression dictionary | **complete** |
| [`\|TTLBTREE`](#ttlbtree-article-titles) | article titles | complete |
| [Topic entries](#topic-entries) | article bodies | **text complete**; record structure partly open |
| [FTC / FTT / FIF](#ftc--ftt--fif-images) | fractal-compressed images | complete (decoder) |
| [`.RLE` baggage](#rle-baggage-files) | inline article graphics | complete |

Tools: `tools/m20dump` (container), `tools/mvbtext` (titles, phrases, topics),
`tools/recomp` (images), `tools/ftcdecode` (clean-room image decoder).

---

## M20 / MVB 2.0 container

A `.M20` is the WinHelp Internal File System (WHIFS): a header, a blob of
internal files, and a B-tree directory at the end mapping names to offsets.

```
[0x00 .. 0x30)              file header (48 bytes)
[0x30 .. dir_start)         internal file data
[dir_start .. end)          B-tree directory
```

### File header (48 bytes at 0)

| Off | Type | Field | Notes |
|-----|------|-------|-------|
| 0  | u32 | `magic` | `0x01045F3F` for MVB 2.0 |
| 4  | u32 | `dir_start` | offset of the B-tree directory |
| 8  | u32 | `first_free` | first free block, 0 if none |
| 12 | u32 | `internal_hdr` | internal file header size (0x28) |
| 20 | u32 | `file_size` | total size in bytes |
| 28 | u32 | `dir_size` | `file_size - dir_start` |
| 40 | u32 | `content_info` | content region size (not fully pinned down) |

Offsets 16, 24, 32, 36, 44 are zero in every file examined.

### B-tree directory

A 48-byte header at `dir_start`, then fixed-size pages (`page_size`, normally
8192 bytes) numbered from 0.

| Off | Type | Field | Notes |
|-----|------|-------|-------|
| 0  | u16 | `magic` | `0x293B` |
| 2  | u16 | `flags` | `0x0102` for an MVB directory |
| 4  | u16 | `page_size` | usually `0x2000` |
| 6  | char[16] | `format` | `"VOO1"` in Encarta 97 |
| 22 | u32 | zero | |
| 26 | u32 | `page_splits` | splits during construction |
| 30 | u32 | `root_page` | |
| 34 | i32 | `-1` | |
| 38 | u32 | `total_pages` | |
| 42 | u16 | `num_levels` | 1 = flat, 2 = root + leaves |
| 44 | u32 | `total_entries` | entries across all leaves |

**Index page** (8-byte header): `u16 flags`, `u16 num_entries`,
`u32 first_child`; then entries of `u8 key_len`, `char key[key_len]`,
`u32 child_page`.

**Leaf page** (12-byte header): `u16 flags`, `u16 num_entries`,
`i32 prev_page`, `i32 next_page`; then entries of

```
u8     key_len
char   key[key_len]        not NUL-terminated
varint file_offset         absolute offset in the .M20
varint file_size           bytes
u8     flags               0x00 in all observed data
```

`varint` is LEB128: 7 bits of payload per byte, bit 7 set means another byte
follows, least-significant group first.

`file_offset` points straight at the data - there is no per-file header to skip.

### Internal file names

| Name | Contents |
|---|---|
| `\|SYSTEM` | configuration, DLL and keyword registrations |
| `\|TOPIC` | topic stream (where present) |
| `\|Phrases` | phrase-compression dictionary |
| `\|CONTEXT` | context number to topic offset |
| `\|TTLBTREE` | article titles |
| `\|FONT`, `\|STOP0` | fonts, search stopwords |
| `>XXXXXXXX` / `\|XXXXXXXX` | topic entries, 8 hex digits |
| `TXXXXXXXA.RLE` etc. | baggage: inline images and other embedded files |

`m20dump` rewrites the leading `|` to `_` so the names are valid on Windows.

Encarta 97 `ENCARTA.M20`: 32,535 internal files, 135 leaf pages.
`PICON.M20`: 11,348 `.FSM` images plus 49 shared `.FTT` tables.

---

## `|Phrases`: the phrase dictionary

Viewer titles shrink their text by replacing common substrings with 2-byte
references into a shared dictionary. This is that dictionary, and it decodes
cleanly:

| Off | Type | Field | Encarta 97 |
|-----|------|-------|------------|
| 0  | u16 | flags / version | `0x0801` |
| 2  | u16 | **`count`** | 1808 phrases |
| 4  | u16 | compression marker | `0x0100` = text is LZ77 compressed |
| 6  | u32 | **`unpacked_size`** | 14901 bytes |
| 10 | u16 | | `0x0020` |
| 12..0x27 | | zero padding | |
| 0x28 | u16[count+1] | **offset table** | |
| 0x28 + `offsets[0]` | | phrase text, LZ77 | |

The offsets are **relative to 0x28**, the start of the table itself, so

```
offsets[0] == 2 * (count + 1)
```

always - a free self-check that pins `count` without guessing. Phrase *i* is
`text[offsets[i] - offsets[0] : offsets[i+1] - offsets[0]]` in the decompressed
text, and `offsets[count] - offsets[0]` equals `unpacked_size` exactly.

### LZ77

The same byte-oriented LZ77 WinHelp uses. Read a control byte, then walk its
bits **least-significant first**:

- bit clear: copy one literal byte
- bit set: read a 2-byte little-endian code; copy `((code >> 12) & 0x0F) + 3`
  bytes from `(code & 0x0FFF) + 1` bytes back in the output

In Encarta 97 this expands 8,531 compressed bytes to exactly the 14,901 the
header predicts.

### What is in it

Sorted, and revealing about the compressor: the first ~200 entries are the
commonest English **bigrams** (`th`, `he`, `in`, `re`, `er`, `on`, `or`, `an`,
`at`, `ti` ...), and the rest are whole words and short phrases in alphabetical
order - `fundamental`, `further`, `gained`, ... `years.`, `young`. Entries
carry their own punctuation, so `government`, `government,` and `government.`
are three separate phrases.

```bash
py tools/mvbtext/mvbtext.py <extract-dir> phrases
```

### Referencing phrases

See [Topic entries](#topic-entries): in decompressed topic text a byte with the
high bit set is a phrase reference, `phrase = byte - 0x80` for low indices.

Note that this is **not** the WinHelp 3.1 scheme (a byte in 1..15 introducing a
2-byte reference, `phrases[(256*(b-1) + next) / 2]`). That is what the format's
ancestry suggests and it is wrong here - applying it to Encarta topic text
produces plausible-looking word salad, which is a good reminder to verify
against the bytes rather than against the documentation of the parent format.

---

## `|TTLBTREE`: article titles

A B-tree of title strings; index nodes repeat keys, so deduplicate while
scanning. Encarta 97 holds **31,517** unique article titles.

```bash
py tools/mvbtext/mvbtext.py <extract-dir> titles
py tools/mvbtext/mvbtext.py <extract-dir> grep Einstein
```

---

## Topic entries

Article bodies live in the numeric internal files (`_XXXXXXXX`). They are
**LZ77 compressed**, with phrase references in the decompressed text.

### Layout

```
[0 .. N)      header + records (not fully understood)
[N .. end)    LZ77 stream, same encoder as |Phrases
```

No header field has been found that points at `N`, so `mvbtext` locates the
stream by trying each start offset and keeping the one that decompresses
furthest. A wrong start hits an impossible back-reference within a few bytes,
so the right one stands out unambiguously - in `_00006060` (Russia) the stream
begins at 2249 and expands 67,466 input bytes into 94,152. A second, shorter
stream sits earlier in the file (offset 1209) holding fact-box and caption
text.

That the same LZ77 decoder byte-exactly reproduces `|Phrases` and also turns
topic bytes into English is the evidence this is right, and it is what the
`00` control byte every ninth byte in a run of text looks like:

```
00 "Great Kr"  00 "emlin Pa"  00 "lace, Mo" ...
```

### Phrase references in topic text

Bytes `0x20`-`0x7E` are literal. Bytes with the high bit set reference
`|Phrases`. **Single-byte references are confirmed** - `phrase = byte - 0x80`:

| Bytes | Decodes as | Result |
|---|---|---|
| `Sib` `84` `ia` | "Sib" + phrase 4 (`er`) + "ia" | **Siberia** |
| `ext` `9C` `c` `89` `85` | "ext" + `ra` + "c" + `ti` + `on` | **extraction** |

**High indices are a two-byte form, and it is solved.** The full encoding:

| Byte | Meaning |
|---|---|
| `0x20`-`0x7E` | literal |
| `0x80`-`0x9F` | phrase `b & 0x1F` - the 32 two-letter fragments |
| `0xA0`-`0xBF` | phrase `((b & 0x0F) << 8) \| next`, **two bytes**, plus a trailing space when `b & 0x10` |

What made this hard is that a legitimate pair of consecutive single-byte
references (`ti` + `on`) is indistinguishable from a two-byte code by
inspection, so the structure cannot be read off an example. **Frequency settled
it.** Over 400 topics the commonest codes decode as:

| Code | Index | Count | Phrase |
|---|---|---|---|
| `B6 6D` | 0x66D | 1,645 | `the` |
| `A4 C8` | 0x4C8 | 1,224 | `of ` |
| `A3 C6` | 0x3C6 | 966 | `in ` |
| `B1 A1` | 0x1A1 | 417 | `and` |

Those are the four commonest words in English, in order. Reading the low nibble
of the first byte as the top four bits of a 12-bit index is the only
arrangement that puts them there, and 1,808 phrases need exactly 11 bits.

Bit `0x10` being a trailing space is why `of ` and `in ` carry their own space
in the dictionary while `the` and `and` do not - the encoder picked whichever
form was shorter in context.

The earlier reading of this section was wrong in an instructive way: it took
`0xB0`-`0xB6` at their single-byte value, found index 54 (`According`) decoding
2,918 times in one article, and concluded the *dictionary* broke at index 48.
The implausible frequency was the clue, but it pointed at the encoding, not the
dictionary.

### What comes out today

```bash
py tools/mvbtext/mvbtext.py <extract-dir> prose _00006060     # Russia
```

Running article prose. From the Russia article, verbatim:

> ...25, 1991 ... 6 krays, 10 okrugs ... 49 oblasts ... Rossiyskaya Federatsiya
> ... Commonwealth of Independent States (CIS) ... 17,075,200 sq km (6,592,800
> sq mi) ... Moscow (1755), Petersburg (1819), Kazan (1804), Novosibirsk (1959)

`tools/regress.py` checks two articles (Russia and `A`) still come out as prose
on every run.

### Remaining work

1. Parse the topic entry header so the LZ77 stream offset is read rather than
   searched for. `topic_stream` currently finds it by trying every offset.
2. The rest of the record structure. Records are NUL-separated (five NULs open
   a heading, three close it) and the media list reads as data - 1,001
   references across 120 topics - but **138 of 240 record types are still
   recognised only by their byte profile**, not parsed.

An alternative to (2), mirroring the DECO_32 recompilation: use the MVB engine
DLLs (`MVBK20N.DLL` / `MVMG20N.DLL`, registered in `|SYSTEM`) as an oracle -
call them to expand topics and validate a clean-room decoder against their
output, rather than inferring the structure.

---

## FTC / FTT / FIF images

Encarta's photographs are fractal-compressed by a codec Microsoft licensed from
Iterated Systems (`DECO_32.DLL`). Full details, including the two encoding
modes found in shipping content, are in
[`tools/recomp/README.md`](../tools/recomp/README.md); the clean-room decoder is
[`tools/ftcdecode`](../tools/ftcdecode).

- **FTC** (`FTC\0`) - fractal-compressed image. Two modes ship in Encarta 97:
  self-contained (`01 01 02 01`, ~29% of PICON) and FTT-referenced
  (`04 03 04 01`, ~71%), the latter sharing transform tables across images.
- **FTT** - raw pixel / fractal transform table, referenced by FTC images in
  the same container.
- **FIF** - container wrapping an embedded FTT or FTC.
- Colour is GBR 4:2:0 (full-resolution green/luma, half-resolution blue/red),
  4x4 blocks, affine transform `out = pixel * 3/4 + bias`.

The recompiled codec in `tools/recomp` decodes both modes and is byte-exact
against the original DLL, with no DLL present at runtime.

---

## `.RLE` baggage files

Despite the name, the `.RLE` entries in `ENCARTA.M20` are **plain uncompressed
24-bit Windows BMPs** - `BM` magic, `biCompression = 0`. They are the inline
article graphics: fact-box art, diagrams, and text-as-image tables such as
grammar charts. Rename to `.bmp` and open; no decoding required.

Article photographs are a different thing entirely and live in `PICON.M20` as
FTC. A topic references both.
