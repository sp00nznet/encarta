#!/usr/bin/env python3
"""
mvbtext - extract article titles and (partial) body text from an ENCARTA.M20
(Microsoft Multimedia Viewer 2.0 / WinHelp-derived) title.

Run m20dump first to extract the internal files:
    m20dump -x ENCARTA.M20 -o <dir>

Then:
    py mvbtext.py <dir> titles            # all article titles (_TTLBTREE)
    py mvbtext.py <dir> text <entry>      # literal text + image refs of a topic
    py mvbtext.py <dir> grep <substr>     # find titles containing substr

Status: titles are fully extractable; topic bodies are literal-text + phrase
references. This tool emits the literal text (proper nouns, section titles,
captions, media/image references) which already identifies articles and links
them to images (decodable via tools/recomp). Full prose needs phrase expansion
from |Phrases, whose offset index is a packed MVB variant — see README.md; the
robust path is the MVB-engine-DLL oracle (mirroring the DECO_32 recompilation).
"""
import os, re, sys


def titles(d):
    """All readable, NUL/printable-delimited title strings from _TTLBTREE."""
    data = open(os.path.join(d, "_TTLBTREE"), "rb").read()
    out, cur, seen = [], bytearray(), set()
    for b in data:
        if 0x20 <= b < 0x7f:
            cur.append(b)
        else:
            if len(cur) >= 3:
                s = cur.decode("latin-1")
                if s not in seen:        # _TTLBTREE repeats titles in index nodes
                    seen.add(s); out.append(s)
            cur = bytearray()
    return out


def literal_text(topic):
    """Topic stream -> literal text. Bytes 1-15 are 2-byte phrase refs (elided
    to a space); 0x20-0x7e are literal; other control bytes dropped."""
    out = bytearray(); i = 0; n = len(topic)
    while i < n:
        b = topic[i]; i += 1
        if 1 <= b <= 15:
            i += 1
            out.append(0x20)
        elif 0x20 <= b < 0x7f:
            out.append(b)
        else:
            out.append(0x20)
    s = re.sub(r"\s{2,}", " ", out.decode("latin-1")).strip()
    return s


def image_refs(text):
    """Embedded media/image references inside a topic's literal text."""
    refs = re.findall(r"\b[0-9A-Za-z]+\.(?:RLE|GRP|FSM|BMP|DIB|WMF)\b", text)
    media = re.findall(r"ENCEW\s+MEDIA97\s+R\d+\s+\d+", text)
    return sorted(set(refs)), media


def main():
    if len(sys.argv) < 3:
        print(__doc__); return 1
    d, cmd = sys.argv[1], sys.argv[2]
    if cmd == "titles":
        ts = titles(d)
        print(f"# {len(ts)} article titles")
        for t in ts:
            print(t)
    elif cmd == "grep":
        sub = sys.argv[3].lower()
        for t in titles(d):
            if sub in t.lower():
                print(t)
    elif cmd == "text":
        topic = open(os.path.join(d, sys.argv[3]), "rb").read()
        txt = literal_text(topic)
        refs, media = image_refs(txt)
        print(f"# {sys.argv[3]}: {len(topic)} bytes -> {len(txt)} chars literal text")
        if refs:  print("# image refs:", ", ".join(refs))
        if media: print("# media refs:", ", ".join(media))
        print(txt)
    else:
        print(__doc__); return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
