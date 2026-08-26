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

## Topic records

A topic is not a flat run of text. Records are separated by runs of NUL, and
the runs mean something - five introduce a section heading, three end it:

```
...unmatched by their felsic rocks. 00000 Physiographic Regions 000 To the
east of the Urals the plain region continues...
```

`records()` splits on them; `topic_prose()` keeps the ones holding text. The
Russia article is 212 records, of which 86 are prose.

**The rest are structure, and they are why `prose` used to trail off.** After
the body comes the media list - what links the article to its pictures, audio
and video. Run through the phrase decoder those produce fluent-looking
nonsense: `thherring`, `ck, ck,`, `irsractith`, for tens of kilobytes. In the
Russia article, junk markers drop from 134 occurrences to 2, and across the
twenty largest topics vowel-less "words" fall from 0.17% to 0.06% while 81% of
the text is retained.

**Classified on the raw bytes, not on what they expand to.** Expanding first is
the obvious approach and it fails: nonsense made of letters passes any test for
letters, which is why the first attempt at this changed nothing. The raw
profile separates them:

```
prose records       0-6%  control bytes, 31-48% phrase references
structure records  10-42% control bytes, or under 30% references
```

Both halves are needed - some structure records are mostly high bytes and give
themselves away by their control bytes, while the media list is the other way
round, largely literal text with few references.

This is a heuristic and is labelled as one in the code: no record type byte has
been found. It costs a few short records, including a 77-byte cross-reference
note that scores 23% control bytes and goes with them.

## The media list

Looking for that type byte turned up something better. The structure records
are not one kind: 102 of 240 begin with the literal bytes `ENC`, and those are
not binary at all - they are NUL-separated fields:

```
"ENCEW" 00 "MEDIA97EW" 00 "R041133 I Red Square, Moscow" 00 ...
```

a collection, a media database, then an ID, a one-letter kind and a caption.
The NULs are field separators; running the phrase decoder over them is what
produced `thherring` and `ck, ck,`.

So `media_records()` reads them instead of discarding them, and the article's
media list comes out as data:

```
# media  image  R041133   Red Square, Moscow
# media  audio  R034589   Borodin's Polovtsian Dances
# media  audio  R035706   Traditional Throat Singing of Tyva
# media  image  R054777   Kirov Ballet
```

Across 120 topics that is 1,001 references - 725 images, 123 audio, 49 maps,
39 tables, 28 charts, 4 video. An unrecognised kind letter passes through as
itself rather than being dropped, so nothing is silently lost.

That leaves two things unread: the remaining 138 structure records, which
expand to repeated fragments and are still only recognised by their byte
profile, and the topic entry's own header - `topic_stream` still finds the LZ77
stream by trying every offset because no field pointing at it has been found.
The header is not compressed and holds a tagged table of the same media
references, with strings split by control bytes (`MVIMG` + `AGE`, `Suzda` +
`l&rsquo;`).

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
