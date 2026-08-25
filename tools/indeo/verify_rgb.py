"""Check the codec's own 24bpp output - the picture ICM_DECOMPRESS hands back.

verify_frames.py reads the decoded plane out of the codec's working buffers,
which proves the decoder. This checks the whole pipeline instead: the driver
messages, the colour conversion, and the DIB the caller actually receives.

24bpp because that is what the codec asks for. ICM_DECOMPRESS_GET_FORMAT
answers 216x192 24bpp BI_RGB, and the other depths are its own conversions
from the same planes.

The comparison is a correlation, not a byte match, and deliberately so: the
reference is ffmpeg's luma plane while this is RGB, so the two are related by
a colour matrix and a round trip through 8-bit RGB. Correlation is 1.0000 when
the picture is right; byte equality would never hold and would say nothing.

    python verify_rgb.py [--limit N]
"""
import argparse
import glob
import os
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
W, H = 216, 192


def corr(a, b):
    a = np.asarray(a, float); b = np.asarray(b, float)
    a = a - a.mean(); b = b - b.mean()
    n = np.sqrt((a * a).sum() * (b * b).sum())
    return float((a * b).sum() / n) if n else 0.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=0)
    a = ap.parse_args()
    files = sorted(glob.glob(os.path.join(AVIDIR, "*.AVI")))
    if a.limit:
        files = files[:a.limit]
    tmp = tempfile.mkdtemp(prefix="ir32rgb")
    good = total = 0
    for path in files:
        out = subprocess.run(
            [os.path.join(FFDIR, "ffprobe"), "-v", "error", "-select_streams", "v",
             "-show_entries", "packet=size,pos", "-of", "csv=p=0", path],
            capture_output=True, text=True).stdout.splitlines()
        if not out:
            continue
        size, pos = (int(x) for x in out[0].split(",")[:2])
        fr = os.path.join(tmp, "f.bin")
        open(fr, "wb").write(open(path, "rb").read()[pos:pos + size])

        ref_path = os.path.join(tmp, "ref.yuv")
        subprocess.run([os.path.join(FFDIR, "ffmpeg"), "-v", "error", "-y", "-i", path,
                        "-pix_fmt", "yuv410p", "-frames:v", "1", "-f", "rawvideo",
                        ref_path], capture_output=True)
        if not os.path.exists(ref_path):
            continue
        ref = np.frombuffer(open(ref_path, "rb").read()[:W * H],
                            np.uint8).reshape(H, W).astype(float)

        ppm = os.path.join(tmp, "o.ppm")
        if os.path.exists(ppm):
            os.remove(ppm)
        subprocess.run([RUN, DLL, "decode", fr, str(W), str(H), ppm, "24"],
                       capture_output=True)
        if not os.path.exists(ppm):
            print("%-16s no output" % os.path.basename(path))
            total += 1
            continue
        raw = open(ppm, "rb").read()
        i = raw.index(b"255\n") + 4
        px = np.frombuffer(raw[i:i + W * H * 3], np.uint8).reshape(H, W, 3).astype(float)
        c = corr(px @ [0.299, 0.587, 0.114], ref)
        total += 1
        good += c > 0.999
        print("%-16s correlation %+.4f%s"
              % (os.path.basename(path), c, "" if c > 0.999 else "   <-- not the picture"))
    print("\n%d of %d frames come back through ICM_DECOMPRESS as the picture"
          % (good, total))
    return 0 if good == total else 1


if __name__ == "__main__":
    sys.exit(main())
