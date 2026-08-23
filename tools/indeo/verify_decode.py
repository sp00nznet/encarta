#!/usr/bin/env python3
"""
verify_decode.py - check the lifted codec's output against ffmpeg's.

A decoder that runs, returns success and fills buffers with plausible bytes
looks exactly like one that works, right up until the bytes are compared with
something known to be right. This does that comparison, and it exists because
skipping it produced a wrong conclusion once already: buffers full of non-zero
data were read as decoded planes and turned out to be verbatim copies of the
codec's own code segment, which the driver copies into a selector it can
execute.

The decoder's internal layout - stride, borders, plane order - is its own
business, so nothing is assumed about it. ffmpeg decodes the same frame to
yuv410p, and a correctly decoded plane has to contain those exact rows
somewhere. Finding one fixes the offset and the stride follows.

    py verify_decode.py <dump-prefix> <ref.yuv> <width> <height>

where the dumps come from `ir32_run ... decode` with IR32_DUMP set, and the
reference from

    ffmpeg -i clip.avi -frames:v 1 -pix_fmt yuv410p -f rawvideo ref.yuv
"""
import argparse
import glob
import os
import sys

try:
    import numpy as np
except ImportError:
    np = None


def load_ref(path, w, h):
    cw, ch = w // 4, h // 4
    d = open(path, "rb").read()
    want = w * h + 2 * cw * ch
    if len(d) != want:
        print("reference is %d bytes, expected %d for %dx%d yuv410p"
              % (len(d), want, w, h))
    return {"Y": (d[:w * h], w, h),
            "U": (d[w * h:w * h + cw * ch], cw, ch),
            "V": (d[w * h + cw * ch:w * h + 2 * cw * ch], cw, ch)}


def exact_search(buf, plane, w, h):
    """Find the plane by its first row, then confirm the rest at that stride."""
    out, start = [], 0
    while True:
        i = buf.find(plane[:w], start)
        if i < 0 or len(out) > 64:
            break
        start = i + 1
        j = buf.find(plane[w:2 * w], i + 1, i + 8192)
        if j < 0:
            continue
        stride = j - i
        ok = sum(1 for r in range(h)
                 if buf[i + r * stride:i + r * stride + w] == plane[r * w:(r + 1) * w])
        out.append((ok, i, stride))
    return out


def near_search(buf, plane, w):
    """Best mean absolute difference for the first row anywhere in the buffer.

    Reported alongside the buffer's own distribution, because a low score on
    low-variance data means nothing on its own: a run of near-constant bytes
    matches a near-constant chroma row by coincidence, and that is exactly what
    the first attempt at this mistook for a hit."""
    if np is None:
        return None
    b = np.frombuffer(buf, np.uint8)
    row = np.frombuffer(plane[:w], np.uint8).astype(np.int16)
    win = np.lib.stride_tricks.sliding_window_view(b, w).astype(np.int16)
    mad = np.abs(win - row).mean(axis=1)
    return float(mad.min()), int(np.argmin(mad)), float(np.percentile(mad, 5))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("prefix", help="dump prefix, e.g. build/buf")
    ap.add_argument("ref")
    ap.add_argument("width", type=int)
    ap.add_argument("height", type=int)
    a = ap.parse_args()

    ref = load_ref(a.ref, a.width, a.height)
    dumps = sorted(glob.glob(a.prefix + "_*.bin"))
    if not dumps:
        print("no dumps matching %s_*.bin" % a.prefix)
        return 2
    print("%d dumps, reference planes Y/U/V = %d/%d/%d bytes"
          % (len(dumps), len(ref["Y"][0]), len(ref["U"][0]), len(ref["V"][0])))

    found = False
    for path in dumps:
        buf = open(path, "rb").read()
        for name, (plane, w, h) in ref.items():
            for ok, off, stride in exact_search(buf, plane, w, h):
                found = True
                print("%s %s: %d/%d rows EXACT at 0x%04X stride %d"
                      % (os.path.basename(path), name, ok, h, off, stride))
    if found:
        return 0

    print("\nno exact row found in any dump. nearest, with the buffer's own "
          "distribution for scale:")
    print("%-16s %-3s %-9s %-8s %s" % ("dump", "pl", "best MAD", "offset", "5th pct"))
    best = None
    for path in dumps:
        buf = open(path, "rb").read()
        for name, (plane, w, h) in ref.items():
            r = near_search(buf, plane, w)
            if r is None:
                continue
            mad, off, p5 = r
            print("%-16s %-3s %-9.2f 0x%04X   %.2f"
                  % (os.path.basename(path), name, mad, off, p5))
            if best is None or mad < best[0]:
                best = (mad, path, name)
    print()
    if best and best[0] < 2:
        print("nearest is %.2f - close enough to be worth checking by hand" % best[0])
    else:
        print("nothing resembles the reference: the planes are not in these "
              "buffers, in any layout")
    return 1


if __name__ == "__main__":
    sys.exit(main())
