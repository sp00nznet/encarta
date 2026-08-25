"""Does any window of a working buffer correlate with ffmpeg's luma?

Byte-exact matching answers "is this the frame". Correlation answers a weaker
but more useful question while the pipeline is still wrong: "is this the frame's
STRUCTURE, in some scaling or layout". A decoder producing a coarse or scaled
representation would fail the first test and pass the second, and the two need
very different follow-ups.
"""
import glob
import numpy as np

W, H = 216, 192
ref = np.frombuffer(open("ref_f0.yuv", "rb").read()[:W * H], np.uint8).reshape(H, W)
refc = ref.astype(np.float64) - ref.mean()
refn = np.sqrt((refc * refc).sum())

STRIDES = [162, 176, 192, 208, 216, 224, 240, 256, 264, 288, 320, 432]
best = []
for path in sorted(glob.glob("k_0*.bin")):
    buf = np.frombuffer(open(path, "rb").read(), np.uint8)
    for stride in STRIDES:
        need = stride * (H - 1) + W
        if need > len(buf):
            continue
        # step the offset coarsely; a real plane will show up as a broad peak,
        # not a single lucky alignment
        for off in range(0, len(buf) - need, 64):
            idx = (np.arange(H)[:, None] * stride) + np.arange(W)[None, :] + off
            win = buf[idx].astype(np.float64)
            sd = win.std()
            if sd < 1.0:
                continue
            wc = win - win.mean()
            r = float((wc * refc).sum() / (np.sqrt((wc * wc).sum()) * refn))
            best.append((abs(r), r, path, off, stride))
best.sort(reverse=True)
print("%-14s %-8s %-7s %s" % ("buffer", "offset", "stride", "correlation"))
for _, r, path, off, stride in best[:10]:
    print("%-14s 0x%04X   %-7d %+.3f" % (path, off, stride, r))
if best and best[0][0] > 0.5:
    print("\nstrong: the structure of the frame is present")
elif best and best[0][0] > 0.25:
    print("\npartial: something frame-shaped, but not cleanly")
else:
    print("\nnothing correlates - the frame's structure is not in these buffers")
