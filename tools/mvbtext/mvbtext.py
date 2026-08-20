#!/usr/bin/env python3
"""
mvbtext - extract article titles, the phrase dictionary, and (partial) body
text from an ENCARTA.M20 (Microsoft Multimedia Viewer 2.0 / WinHelp-derived).

Run m20dump first to extract the internal files:
    m20dump -x ENCARTA.M20 -o <dir>

Then:
    py mvbtext.py <dir> titles            # all article titles (_TTLBTREE)
    py mvbtext.py <dir> phrases           # the |Phrases dictionary, decoded
    py mvbtext.py <dir> prose <entry>     # decompress a topic and read it
    py mvbtext.py <dir> text <entry>      # raw (undecompressed) topic scan
    py mvbtext.py <dir> grep <substr>     # find titles containing substr

Status: titles and |Phrases are complete. Topic bodies are **LZ77 compressed**
(the same WinHelp LZ77 that packs the phrase dictionary) and `prose` reads real
article text out of them. What is not finished is the encoding of high phrase
indices inside a topic, so some references still come out wrong - see
docs/FORMATS.md for exactly what is known and what is not.
"""
import os, re, struct, sys


# ---------------------------------------------------------------- |Phrases

def lz77(src, cap=None):
    """WinHelp/MVB LZ77. Control byte, LSB-first: a set bit introduces a 2-byte
    (len<<12)|dist code copying len+3 bytes from dist+1 back; a clear bit is a
    literal byte. Stops cleanly on an impossible back-reference, which is what
    makes it usable as a probe for where a stream starts."""
    out = bytearray()
    i, n = 0, len(src)
    while i < n:
        if cap and len(out) >= cap:
            break
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

def topic_stream(raw, scan_limit=4096):
    """A topic entry is a header we do not fully understand followed by an LZ77
    stream. No header field has been found that points at the stream, so locate
    it by trying each start and keeping the one that decompresses furthest -
    a wrong start hits an impossible back-reference almost immediately, so this
    is cheap and unambiguous in practice.

    Returns (decompressed_bytes, start_offset)."""
    best = (0, 0)
    for start in range(min(scan_limit, len(raw))):
        out = lz77(raw[start:], cap=4096)
        if len(out) > best[0]:
            best = (len(out), start)
    if best[0] == 0:
        return b"", -1
    return bytes(lz77(raw[best[1]:])), best[1]


def expand_refs(buf, ph):
    """Topic text -> readable text.

    Inside a decompressed topic, bytes 0x20-0x7E are literal and bytes with the
    high bit set reference the phrase dictionary. Single-byte references
    (phrase = byte - 0x80) are confirmed: `Sib<84>ia` is "Siberia" (phrase 4 =
    "er") and `ext<9C>c<89><85>` is "extraction" ("ra" + "ti" + "on").

    High phrase indices are NOT solved. All 128 high bytes occur, and the most
    frequent (0xB0-0xB6) cannot be their single-byte reading - phrase 54 is
    "According", which is not plausible thousands of times in one article. They
    are either a multi-byte escape or interleaved formatting codes. Until that
    is settled this renders those bytes as their single-byte phrase, which is
    why some words come out wrong."""
    out = []
    for b in buf:
        if b >= 0x80:
            idx = b - 0x80
            out.append(ph[idx] if ph and idx < len(ph) else "")
        elif 0x20 <= b < 0x7F or b in (9, 10, 13):
            out.append(chr(b))
        else:
            out.append(" ")
    return re.sub(r"[ \t]{2,}", " ", "".join(out)).strip()


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
    elif cmd == "prose":
        raw = open(os.path.join(d, sys.argv[3]), "rb").read()
        try:
            ph = phrases(d)
        except Exception as e:
            print(f"# no phrase dictionary ({e})", file=sys.stderr); ph = None
        stream, start = topic_stream(raw)
        if start < 0:
            print("# no LZ77 stream found"); return 1
        txt = expand_refs(stream, ph)
        refs, media = image_refs(txt)
        print(f"# {sys.argv[3]}: {len(raw)} bytes -> LZ77 stream at {start} "
              f"-> {len(stream)} bytes -> {len(txt)} chars")
        if refs:  print("# image refs:", ", ".join(refs))
        if media: print("# media refs:", ", ".join(media))
        print(txt)
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
