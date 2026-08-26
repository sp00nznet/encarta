# mvbtext — Encarta article text/title extractor (MVB 2.0)

`ENCARTA.M20` is a **Microsoft Multimedia Viewer 2.0** title (a superset of the
WinHelp 3.1 `.HLP` format; `|SYSTEM` magic `0x036C`). This tool reads the
internal files (after `m20dump -x`) to extract article titles and topic text.

```bash
m20dump -x "G:\ENCYC97\ENCARTA.M20" -o enc_dir     # extract internal files
py mvbtext.py enc_dir titles                        # ~31,517 article titles
py mvbtext.py enc_dir grep Einstein                 # titles matching a substring
py mvbtext.py enc_dir text _00006060                # literal text + image refs
```

## What works

- **Titles — complete.** All ~31,517 article titles from `_TTLBTREE`
  (e.g. *Aardvark, Abacus, Einstein Albert, Lincoln, Russia, …*).
- **Phrase dictionary — complete.** 1,808 phrases, see below.
- **Topic prose — readable.** Topic entries (`_XXXXXXXX`) are *text + phrase
  references* inside formatted records. The phrase references are solved (see
  below), so `prose` produces running article text, along with **media /
  image references** that link an article to its pictures (e.g. `11280A.RLE`,
  `ENCEW MEDIA97 R04 2420`), decodable via [`../recomp`](../recomp). Example:
  `_00006060` = the **Russia** article; `_000071A0` = the **USSR** article.

## |Phrases: solved

The phrase dictionary **decodes cleanly** - 1,808 phrases, byte-exact against
the size the header declares. Full field layout in
[docs/FORMATS.md](../../docs/FORMATS.md); the short version is that the u16
offset table at 0x28 is relative to 0x28 itself, so `offsets[0] == 2*(count+1)`
is a free self-check, and the text after it is ordinary WinHelp LZ77.

```bash
py mvbtext.py enc_dir phrases          # 1,808 entries
```

Sorted and revealing: the first ~200 entries are the commonest English bigrams
(`th`, `he`, `in`, `re`, `er`, ...), the rest whole words alphabetically
(`fundamental` ... `young`), each carrying its own punctuation so `government`,
`government,` and `government.` are separate entries.

`text` now expands references instead of eliding them.

## Phrase references: solved

Article prose comes out as prose. `_00006060` now reads:

> Russia, independent republic in eastern Europe and Asia, which was
> established on December 25, 1991, and includes 21 ethnically based
> republics, 6 *krays* (territories), 10 *okrugs* (national areas), 49
> *oblasts* (districts), 1 autonomous region, and 2 cities with federal
> status. Officially named the Russian Federation (Russian *Rossiyskaya
> Federatsiya*), Russia was once the largest and most prominent republic of
> the Union of Soviet Socialist Republics...

A reference is one byte or two:

```
0x80-0x9F   phrase (b & 0x1F)                       the 32 two-letter fragments
0xA0-0xBF   phrase ((b & 0x0F) << 8) | next         plus a space when b & 0x10
```

**How the two-byte form was found: frequency.** Over 400 topics the commonest
codes are

```
B6 6D  ->  0x66D = 1645  "the"
A4 C8  ->  0x4C8 = 1224  "of "
B1 A1  ->  0x1A1 =  417  "and"
A3 C6  ->  0x3C6 =  966  "in "
```

the four commonest words in English, in order. Reading the low nibble of the
first byte as the top four bits of a 12-bit index is the only arrangement that
puts them there, and 1,808 phrases need exactly 11 bits. Bit 0x10 is a trailing
space, which is why `of ` and `in ` carry their own space in the dictionary
while `the` and `and` do not - the encoder used whichever form was shorter and
flagged the rest.

Reading every high byte as a single reference, which is what this did before,
can only reach 128 of the 1,808 phrases and lands in the alphabetical section:
`0xB6` came out as *According*, thousands of times per article. Its being
implausible was the clue; frequency was what settled it.

First bytes are 0xA0-0xBF. Over 1,500 topics that range decodes 99.8%
in-range against the dictionary while 0xC0-0xFF manages 12-78%, so those bytes
are not this code; they are 2% of all high bytes, are left alone, and are
counted rather than guessed at.

```bash
py mvbtext.py enc_dir check            # pins the four codes above
py mvbtext.py enc_dir prose _00006060  # the Russia article
```

## What's left: topic records

The prose is right; the surrounding structure is not. A topic entry is a
sequence of formatted records - WinHelp `TOPICLINK` style: block size,
prev/next, record type, then `LinkData1` formatting and `LinkData2` text - and
`prose` runs the phrase decoder over the whole decompressed block, so the
non-text regions come out as repeated fragments after the article body ends.
Parsing the record layout would separate them. Ref: helpdeco / Winterhoff
`helpfile.txt`.

**Alternative path**, mirroring the DECO_32 recompilation: use the MVB engine
DLLs (`MVBK20N.DLL` / `MVMG20N.DLL`, registered in `_SYSTEM`) as an oracle -
call them to expand topics and validate a clean-room parser against their
output.

## Inline images are plain BMPs

The `.RLE` entries in ENCARTA.M20 are **standard uncompressed 24-bit Windows
BMPs** (`BM` magic, `biCompression=0`) despite the name — article inline
graphics, fact-box art, and text-as-image tables (e.g. grammar/conjugation
charts). Just rename `<name>.RLE` → `.bmp` to view; no decoding needed. The
encyclopedia's photographs live in `PICON.M20` as FTC (decode via
[`../recomp`](../recomp)); article topics reference both.

So a near-complete article view is already assemblable: **title** (`_TTLBTREE`) +
**literal text / captions / media refs** (topic entries) + **inline BMP graphics**
(`.RLE`) + **linked FTC photos** (PICON.M20). Only the connective prose (the
phrase-compressed common words) is still missing — see above.

## MVB internal files (m20dump sanitizes `|` → `_`)
`_SYSTEM` (config + DLL/keyword registrations), `_Phrases` (phrase dictionary),
`_TTLBTREE` (titles), `_CONTEXT` (topic-ID → offset), `_FONT`, `_STOP0`
(stopwords); topic bodies in the numeric `_XXXXXXXX` entries; images as `.RLE`/
`.GRP`/`.FSM`.
