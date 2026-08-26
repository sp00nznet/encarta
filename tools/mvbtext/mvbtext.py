#!/usr/bin/env python3
"""
mvbtext - extract article titles, the phrase dictionary, and (partial) body
text from an ENCARTA.M20 (Microsoft Multimedia Viewer 2.0 / WinHelp-derived).

Run m20dump first to extract the internal files:
    m20dump -x ENCARTA.M20 -o <dir>

Then:
    py mvbtext.py <dir> titles            # all article titles (_TTLBTREE)
    py mvbtext.py <dir> phrases           # the |Phrases dictionary, decoded
    py mvbtext.py <dir> check             # pin the phrase encoding
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


def expand_refs(buf, ph, stats=None):
    """Decompressed topic text -> readable prose.

    Phrase references are one byte or two:

        0x80-0x9F   phrase (b & 0x1F)          the 32 two-letter fragments
        0xA0-0xBF   phrase ((b & 0x0F) << 8) | next,
                    plus a trailing space when b & 0x10

    The two-byte form is what took the longest to see, and the frequencies
    gave it away. Over 400 topics the commonest codes are

        B6 6D  ->  0x66D = 1645  "the"
        A4 C8  ->  0x4C8 = 1224  "of "
        B1 A1  ->  0x1A1 =  417  "and"
        A3 C6  ->  0x3C6 =  966  "in "

    which are the four commonest words in English, in order. Reading the low
    nibble of the first byte as the top four bits of a 12-bit index is the only
    arrangement that puts them there, and 1,808 phrases need exactly 11 bits.

    Bit 0x10 is a trailing space, which is why "of " and "in " carry their own
    space in the dictionary while "the" and "and" do not - the encoder picked
    whichever form was shorter overall and flagged the rest.

    Reading every high byte as a single reference - which is what this did
    before - can only reach 128 of the 1,808 phrases, and lands in the middle
    of the alphabetical section: 0xB6 came out as "According", thousands of
    times per article.

    First bytes are 0xA0-0xBF: over 1,500 topics that range is 99.8% in-range
    against the dictionary, while 0xC0-0xFF is 12-78% and is therefore not this
    code. Those bytes are left alone (2% of high bytes, still unidentified) and
    counted in `stats` rather than guessed at.
    """
    out = []
    i, n = 0, len(buf)
    while i < n:
        b = buf[i]
        i += 1
        if 0x80 <= b <= 0x9F:
            idx = b & 0x1F
            out.append(ph[idx] if ph and idx < len(ph) else "")
        elif 0xA0 <= b <= 0xBF and i < n:
            idx = ((b & 0x0F) << 8) | buf[i]
            i += 1
            if ph and idx < len(ph):
                out.append(ph[idx] + (" " if b & 0x10 else ""))
            elif stats is not None:
                stats["out_of_range"] = stats.get("out_of_range", 0) + 1
        elif b >= 0xC0:
            if stats is not None:
                stats["unknown_hi"] = stats.get("unknown_hi", 0) + 1
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


def check(d):
    """Pin the phrase encoding to the four codes that identify it.

    These are the commonest codes in the corpus and they decode to the four
    commonest words in English, in order. If a change to expand_refs or to the
    dictionary reader breaks the indexing, this says so immediately instead of
    the damage showing up as slightly-wrong prose thousands of articles later.
    """
    ph = phrases(d)
    cases = [(0xB6, 0x6D, 1645, "the "),      # 0x66D, bit 0x10 adds the space
             (0xA4, 0xC8, 1224, "of "),       # 0x4C8, already spaced
             (0xB1, 0xA1, 417,  "and "),      # 0x1A1
             (0xA3, 0xC6, 966,  "in ")]       # 0x3C6
    ok = True
    for hi, lo, idx, want in cases:
        got = expand_refs(bytes([hi, lo]), ph)
        # expand_refs strips, so compare on the stripped form
        if ((hi & 0x0F) << 8 | lo) != idx or got != want.strip():
            print("  FAIL %02X %02X -> index %d %r, wanted %d %r"
                  % (hi, lo, (hi & 0x0F) << 8 | lo, got, idx, want.strip()))
            ok = False
        else:
            print("  ok   %02X %02X -> phrase %-4d %r" % (hi, lo, idx, want))
    # the single-byte form: "Sib<84>ia" is Siberia, phrase 4 being "er"
    if expand_refs(b"Sib\x84ia", ph) != "Siberia":
        print("  FAIL single-byte reference: %r" % expand_refs(b"Sib\x84ia", ph))
        ok = False
    else:
        print("  ok   Sib<84>ia -> Siberia")
    print("phrase encoding: %s" % ("all checks passed" if ok else "BROKEN"))
    return 0 if ok else 1


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
    elif cmd == "check":
        return check(d)
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
