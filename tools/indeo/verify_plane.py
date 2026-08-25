"""Find the decoded luma plane in a dumped buffer and check it against ffmpeg.

The codec never hands a correct picture back through ICM_DECOMPRESS, because
the colour conversion writes at a pitch that does not match the DIB. But the
plane it decoded is sitting in its own working buffer, and that is the thing
worth checking: it says whether the recompiled decoder actually decodes.

It does. Against ffmpeg's decode of the same packet, with `ours * 2`:

    columns 0..167   100.0% byte-exact, all 192 rows
    columns 168..175  99.8% wrong

So the arithmetic, the codebooks, the bitstream reader and the reconstruction
are all correct, and the decoder is filling 42 dwords of every row where a
216-wide frame needs 54.

The factor of two is the domain: Indeo 3 works in six bits, ffmpeg scales by
four on output and this plane holds twice the six-bit value.

    python verify_plane.py <dumped buffer> <ffmpeg yuv410p> [w] [h]
"""
import sys

import numpy as np


def find_plane(buf, ref, strides=range(160, 240)):
    """Locate the plane by correlation, then confirm it byte-exactly.

    Correlation finds it even when the values are on a different scale, which
    is the situation here - byte matching alone would have found nothing and
    said "not decoded"."""
    h, w = ref.shape
    best = (-2.0, 0, 0)
    for st in strides:
        cw = min(w, st)
        r = ref[:, :cw].astype(float)
        rc = r - r.mean()
        rn = np.sqrt((rc * rc).sum())
        for off in range(0, len(buf) - st * (h - 1) - cw, 4):
            idx = (np.arange(h)[:, None] * st) + np.arange(cw)[None, :] + off
            win = buf[idx].astype(float)
            if win.std() < 2:
                continue
            wc = win - win.mean()
            d = np.sqrt((wc * wc).sum()) * rn
            if d < 1e-9:
                continue
            c = float((wc * rc).sum() / d)
            if c > best[0]:
                best = (c, st, off)
    return best


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    w = int(sys.argv[3]) if len(sys.argv) > 3 else 216
    h = int(sys.argv[4]) if len(sys.argv) > 4 else 192
    buf = np.frombuffer(open(sys.argv[1], "rb").read(), np.uint8)
    ref = np.frombuffer(open(sys.argv[2], "rb").read()[:w * h],
                        np.uint8).reshape(h, w)

    corr, stride, off = find_plane(buf, ref)
    print("plane found at offset 0x%X, stride %d, correlation %.3f"
          % (off, stride, corr))
    cw = min(w, stride)
    idx = (np.arange(h)[:, None] * stride) + np.arange(cw)[None, :] + off
    ours = buf[idx].astype(int) * 2          # six-bit domain, see the docstring
    r = ref[:, :cw].astype(int)

    exact = (ours == r)
    print("ours*2 vs the reference over %d x %d:" % (cw, h))
    print("   byte-exact: %d of %d (%.1f%%)"
          % (exact.sum(), exact.size, 100.0 * exact.mean()))
    # where the disagreement is matters more than how much there is: an even
    # spread would mean a broken decode, a block at one edge means a width
    percol = exact.mean(0)
    good = np.nonzero(percol > 0.99)[0]
    if len(good):
        print("   columns %d..%d are 100%% exact; %d..%d are not"
              % (good[0], good[-1], good[-1] + 1, cw - 1))
        print("   -> the decoder fills %d of the %d dwords a row needs"
              % ((good[-1] + 1) // 4, w // 4))
    return 0


if __name__ == "__main__":
    sys.exit(main())
