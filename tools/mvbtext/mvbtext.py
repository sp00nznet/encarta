#!/usr/bin/env python3
"""
mvbtext - extract article titles, the phrase dictionary, and (partial) body
text from an ENCARTA.M20 (Microsoft Multimedia Viewer 2.0 / WinHelp-derived).

Run m20dump first to extract the internal files:
    m20dump -x ENCARTA.M20 -o <dir>

Then:
    py mvbtext.py <dir> titles            # all article titles (_TTLBTREE)
    py mvbtext.py <dir> phrases           # the |Phrases dictionary, decoded
    py mvbtext.py <dir> text <entry>      # topic text with phrases expanded
    py mvbtext.py <dir> grep <substr>     # find titles containing substr

Status: titles are fully extractable. |Phrases is fully decoded (see
docs/FORMATS.md) so phrase references now expand to their real text instead of
being elided. Topic bodies still are not running prose: expanding a topic
stream byte-for-byte yields real phrases and real captions but interleaved with
record structure, because the topic entries are formatted records rather than a
flat text stream. Parsing those records is the remaining step - see README.md.
"""
import os, re, struct, sys


# ---------------------------------------------------------------- |Phrases

def lz77(src):
    """WinHelp/MVB LZ77. Control byte, LSB-first: a set bit introduces a 2-byte
    (len<<12)|dist code copying len+3 bytes from dist+1 back; a clear bit is a
    literal byte."""
    out = bytearray()
    i, n = 0, len(src)
    while i < n:
        bits = src[i]; i += 1
        for b in range(8):
            if i >= n:
                break
            if bits & (1 << b):
                if i + 1 >= n:
                    break
                code = src[i] | (src[i + 1] << 8); i += 2
                length = ((code >> 12) & 0x0F) + 3
                dist = (code & 0x0FFF) + 1
                start = len(out) - dist
                if start < 0:
                    return out                      # corrupt; return what we have
                for k in range(length):
                    out.append(out[start + k])
            else:
                out.append(src[i]); i += 1
    return out


PHRASE_TABLE = 0x28        # offset table sits at a fixed 40-byte header


def phrases(d):
    """Decode |Phrases -> list of phrase strings.

    Header: u16 flags, u16 count, u16 0x0100 (LZ77 marker), u32 unpacked size.
    At 0x28: u16 offsets[count+1], relative to 0x28 itself, so offsets[0] is
    always 2*(count+1) - a free self-check. The text after the table is LZ77
    compressed and expands to exactly the size in the header.
    """
    data = open(os.path.join(d, "_Phrases"), "rb").read()
    _flags, count, marker = struct.unpack_from("<HHH", data, 0)
    unpacked = struct.unpack_from("<I", data, 6)[0]
    off = [struct.unpack_from("<H", data, PHRASE_TABLE + 2 * i)[0]
           for i in range(count + 1)]
    if off[0] != 2 * (count + 1):
        raise ValueError("phrase table self-check failed: off[0]=%d, expected %d"
                         % (off[0], 2 * (count + 1)))
    text = lz77(data[PHRASE_TABLE + off[0]:]) if marker == 0x0100 \
        else data[PHRASE_TABLE + off[0]:]
    if len(text) != unpacked:
        print("# warning: unpacked %d bytes, header said %d" % (len(text), unpacked),
              file=sys.stderr)
    base = off[0]
    return [text[off[i] - base:off[i + 1] - base].decode("latin-1")
            for i in range(count)]


# ---------------------------------------------------------------- topics

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


def literal_text(topic, ph=None):
    """Topic stream -> text. Bytes 1-15 start a 2-byte phrase reference:
    idx = 256*(b-1) + next, naming phrase idx//2, with a trailing space when
    idx is odd. 0x20-0x7e are literal; other control bytes become spaces.

    With `ph` (the decoded dictionary) references expand to their text; without
    it they collapse to a space, which is what this did before |Phrases was
    decoded."""
    out = []
    i, n = 0, len(topic)
    while i < n:
        b = topic[i]; i += 1
        if 1 <= b <= 15 and i < n:
            idx = 256 * (b - 1) + topic[i]; i += 1
            if ph and idx // 2 < len(ph):
                out.append(ph[idx // 2] + (" " if idx & 1 else ""))
            else:
                out.append(" ")
        elif 0x20 <= b < 0x7f:
            out.append(chr(b))
        else:
            out.append(" ")
    return re.sub(r"\s{2,}", " ", "".join(out)).strip()


def image_refs(text):
    """Embedded media/image references inside a topic's text."""
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
    elif cmd == "phrases":
        ps = phrases(d)
        print(f"# {len(ps)} phrases")
        for i, p in enumerate(ps):
            print(f"{i:5d}  {p}")
    elif cmd == "grep":
        sub = sys.argv[3].lower()
        for t in titles(d):
            if sub in t.lower():
                print(t)
    elif cmd == "text":
        topic = open(os.path.join(d, sys.argv[3]), "rb").read()
        try:
            ph = phrases(d)
        except Exception as e:                     # dictionary optional
            print(f"# no phrase dictionary ({e}); references elided", file=sys.stderr)
            ph = None
        txt = literal_text(topic, ph)
        refs, media = image_refs(txt)
        print(f"# {sys.argv[3]}: {len(topic)} bytes -> {len(txt)} chars"
              f"{' (phrases expanded)' if ph else ''}")
        if refs:  print("# image refs:", ", ".join(refs))
        if media: print("# media refs:", ", ".join(media))
        print(txt)
    else:
        print(__doc__); return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
