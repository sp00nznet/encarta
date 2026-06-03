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
- **Topic bodies — partial (literal text).** Topic entries (`_XXXXXXXX`) are
  *literal text + phrase references*. The literal stream already yields
  identifiable content — proper nouns, section titles, captions, and **media /
  image references** that link an article to its pictures (e.g. `11280A.RLE`,
  `ENCEW MEDIA97 R04 2420`), decodable via [`../recomp`](../recomp). Example:
  `_00006060` = the **Russia** article; `_000071A0` = the **USSR** article.

## What's left: full prose

The common words/phrases are compressed via `|Phrases` (2,049 entries; dictionary
content is readable — months, US states, countries, "Civil War", "President",
"United States", …). The phrase **offset index** is a packed/segmented MVB
variant (header bytes `01 08 10 07 00 01 …`; the table near byte 40 holds
file-offsets `3618, 3620, …` but only part stays in range and entries carry
formatting control bytes), and it did not yield to the classic WinHelp 3.1
layout, LZ77, or brute-force base/count search validated by English readability.

**Recommended path** (mirrors the successful DECO_32 recompilation): use the MVB
engine DLLs (`MVBK20N.DLL` / `MVMG20N.DLL`, registered in `_SYSTEM`) as a
decompression **oracle** — load and call them to expand topics — rather than
reverse-engineering the packed phrase index by inference. Then parse the
topic-block / paragraph structure (formatting, links, fonts) for full rendered
articles. Ref: helpdeco / Winterhoff `helpfile.txt`.

## MVB internal files (m20dump sanitizes `|` → `_`)
`_SYSTEM` (config + DLL/keyword registrations), `_Phrases` (phrase dictionary),
`_TTLBTREE` (titles), `_CONTEXT` (topic-ID → offset), `_FONT`, `_STOP0`
(stopwords); topic bodies in the numeric `_XXXXXXXX` entries; images as `.RLE`/
`.GRP`/`.FSM`.
