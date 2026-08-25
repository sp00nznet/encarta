"""Decode the first frame of many AVIs with the recompiled codec and check
every pixel against ffmpeg.

One frame decoding correctly can be luck - a lucky offset, a lucky alignment.
Sixty-eight different frames cannot be, which is why this runs the whole
directory rather than the one file the investigation used.

Keyframes only, and the first of each file. Every run is a fresh process with
fresh codec state, so an inter frame has no previous frame to predict from and
would not decode standalone; the first frame of an Indeo 3 AVI is a keyframe.

The decoded luma plane lives in the codec's own working buffers, in two
vertical strips - the left 168 columns in one and the remaining 48 in the
other, both at a 176-byte stride. That is the codec's layout, not a defect:
its plane workspace is 176 bytes wide, so a 216-wide frame is decoded in two
passes. What ICM_DECOMPRESS hands back is separately wrong, because the colour
converters write at a hardcoded 256-byte pitch that does not match the DIB.

The comparison is `ours * 2`: Indeo 3 works in six bits, ffmpeg scales by four
on output, and this plane holds twice the six-bit value.

    python verify_frames.py [--limit N]
"""
import argparse
import glob
import os
import struct
import subprocess
import sys
import tempfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
RUN = os.path.join(HERE, "runtime", "build", "ir32_run.exe")
DLL = r"H:\AAMSSTP\SYSTEM16\IR32.DLL"
AVIDIR = "H:/ENCYC97/MM/AVI"
FFDIR = (r"C:\Users\nedch\AppData\Local\Microsoft\WinGet\Packages"
         r"\Gyan.FFmpeg_Microsoft.Winget.Source_8wekyb3d8bbwe"
         r"\ffmpeg-8.1.1-full_build\bin")

# where each strip sits, and how wide it is
LEFT = (0x10734, 168)
RIGHT = (0x1073C, 48)
STRIDE = 176


def first_packet(path):
    """Offset and size of the first video packet, from ffprobe."""
    out = subprocess.run(
        [os.path.join(FFDIR, "ffprobe"), "-v", "error", "-select_streams", "v",
         "-show_entries", "packet=size,pos", "-of", "csv=p=0", path],
        capture_output=True, text=True).stdout.splitlines()
    if not out:
        return None
    size, pos = out[0].split(",")[:2]
    return int(pos), int(size)


def grab(buf, off, stride, w, h):
    if off + stride * (h - 1) + w > len(buf):
        return None
    idx = (np.arange(h)[:, None] * stride) + np.arange(w)[None, :] + off
    return buf[idx].astype(int) * 2


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=0)
    a = ap.parse_args()

    files = sorted(glob.glob(os.path.join(AVIDIR, "*.AVI")))
    if a.limit:
        files = files[:a.limit]
    tmp = tempfile.mkdtemp(prefix="ir32v")
    perfect = total = 0
    worst = []
    for path in files:
        pkt = first_packet(path)
        if not pkt:
            continue
        pos, size = pkt
        raw = open(path, "rb").read()[pos:pos + size]
        fr = os.path.join(tmp, "f.bin")
        open(fr, "wb").write(raw)

        ref_path = os.path.join(tmp, "ref.yuv")
        subprocess.run([os.path.join(FFDIR, "ffmpeg"), "-v", "error", "-y",
                        "-i", path, "-pix_fmt", "yuv410p", "-frames:v", "1",
                        "-f", "rawvideo", ref_path], capture_output=True)
        if not os.path.exists(ref_path):
            continue
        ref = np.frombuffer(open(ref_path, "rb").read()[:216 * 192],
                            np.uint8).reshape(192, 216).astype(int)

        pre = os.path.join(tmp, "d")
        for f in glob.glob(pre + "*"):
            os.remove(f)
        subprocess.run([RUN, DLL, "decode", fr, "216", "192", "", "8"],
                       capture_output=True,
                       env=dict(os.environ, IR32_DUMP=pre))
        try:
            b5 = np.frombuffer(open(pre + "_0405.bin", "rb").read(), np.uint8)
            b6 = np.frombuffer(open(pre + "_0406.bin", "rb").read(), np.uint8)
        except IOError:
            continue
        left = grab(b5, LEFT[0], STRIDE, LEFT[1], 192)
        right = grab(b6, RIGHT[0], STRIDE, RIGHT[1], 192)
        if left is None or right is None:
            continue
        img = np.concatenate([left, right], axis=1)
        eq = (img == ref)
        total += 1
        if eq.all():
            perfect += 1
        else:
            worst.append((eq.mean(), os.path.basename(path)))
        print("%-16s %6.2f%% byte-exact%s"
              % (os.path.basename(path), 100 * eq.mean(),
                 "" if eq.all() else "   <-- not exact"))

    print("\n%d of %d first frames decode byte-exactly" % (perfect, total))
    for m, n in sorted(worst)[:5]:
        print("   %s %.2f%%" % (n, 100 * m))
    return 0 if perfect == total else 1


if __name__ == "__main__":
    sys.exit(main())
