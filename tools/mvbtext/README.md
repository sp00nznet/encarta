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
- **Topic bodies — partial.** Topic entries (`_XXXXXXXX`) are *text + phrase
  references* inside formatted records. The extractable stream yields
  identifiable content — proper nouns, section titles, captions, and **media /
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

## What's left: topic records

Expanding a topic byte-for-byte yields real phrases and real captions, but
interleaved with record structure rather than as running prose - the topic
entries are **formatted records**, not a flat text stream, and whole-file LZ77
is not how they are packed (it expands into zeroes).

So the blocker has moved: it is no longer the dictionary, it is parsing the
topic record layout (WinHelp `TOPICLINK`-style: block size, prev/next, record
type, then `LinkData1` formatting and `LinkData2` text). Ref: helpdeco /
Winterhoff `helpfile.txt`.

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
