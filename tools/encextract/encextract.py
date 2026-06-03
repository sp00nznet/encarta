#!/usr/bin/env python3
"""
encextract - end-to-end Encarta 97 content extractor.

Ties together the project's tools into one pipeline:
  m20dump (-x raw)  ->  recomp_decode (statically-recompiled FTC codec)  ->  PNG
                    ->  mvbtext (MVB 2.0 titles / literal text)

Given a mounted Encarta data disc (the ENCYC97 directory) it produces a browsable
output tree: decoded image gallery (thumbnails + full-size photos), an article
title index, and per-topic literal text — using only recompiled / clean-room code
(no original Encarta code executes in the decode path).

Usage:
  py encextract.py <ENCYC97_dir> <out_dir> [--max-images N]

Build the native tools first:
  cmake --build build --config Release --target m20dump recomp_decode

Requires: Python 3 + Pillow (for the HTML gallery thumbnails; optional).
Decoded content is Microsoft's — keep it local; do not redistribute.
"""
import os, sys, subprocess, glob, html, shutil, argparse

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
M20DUMP = os.path.join(ROOT, "build", "tools", "m20dump", "Release", "m20dump.exe")
RECOMP  = os.path.join(ROOT, "build", "tools", "recomp", "Release", "recomp_decode.exe")
MVBTEXT = os.path.join(ROOT, "tools", "mvbtext", "mvbtext.py")


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True)


def extract(m20, outdir):
    os.makedirs(outdir, exist_ok=True)
    # -x = raw: FTC/FSM/RLE entries are uncompressed; -d (LZ77) would corrupt them
    run([M20DUMP, "-x", m20, "-o", outdir])
    return outdir


def decode_images(srcdir, pngdir, limit):
    os.makedirs(pngdir, exist_ok=True)
    n = 0
    for f in sorted(glob.glob(os.path.join(srcdir, "*"))):
        if n >= limit:
            break
        try:
            magic = open(f, "rb").read(4)
        except OSError:
            continue
        base = os.path.splitext(os.path.basename(f))[0]
        if magic == b"FTC\x00":
            out = os.path.join(pngdir, base + ".png")
            r = run([RECOMP, f, "", out])
            if os.path.exists(out):
                n += 1
        elif magic[:2] == b"BM":            # .RLE entries are plain BMPs
            shutil.copy(f, os.path.join(pngdir, base + ".bmp"))
            n += 1
    return n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("encyc")
    ap.add_argument("out")
    ap.add_argument("--max-images", type=int, default=60)
    a = ap.parse_args()

    for tool in (M20DUMP, RECOMP):
        if not os.path.exists(tool):
            print(f"missing {tool} — build it first (cmake --build ...)"); return 1

    os.makedirs(a.out, exist_ok=True)
    # 1) article titles + structure from the main content file
    enc = os.path.join(a.encyc, "ENCARTA.M20")
    encdir = extract(enc, os.path.join(a.out, "_encarta"))
    tr = run([sys.executable, MVBTEXT, encdir, "titles"])
    open(os.path.join(a.out, "titles.txt"), "w", encoding="utf-8").write(tr.stdout)
    ntitles = tr.stdout.count("\n")
    print(f"[+] {ntitles} article titles -> titles.txt")

    # 2) decode images from PICON (thumbnails) and MAXMED (full-size photos)
    total = 0
    for name in ("PICON.M20", "MAXMED1.M20"):
        src = os.path.join(a.encyc, name)
        if not os.path.exists(src):
            continue
        xdir = extract(src, os.path.join(a.out, "_" + name.split(".")[0].lower()))
        got = decode_images(xdir, os.path.join(a.out, "images"), a.max_images - total)
        print(f"[+] {got} images decoded from {name}")
        total += got

    # 3) simple HTML index
    imgs = sorted(glob.glob(os.path.join(a.out, "images", "*")))
    with open(os.path.join(a.out, "index.html"), "w", encoding="utf-8") as h:
        h.write("<!doctype html><meta charset=utf-8><title>Encarta 97 (recompiled)</title>")
        h.write("<h1>Encarta 97 — decoded via static recompilation</h1>")
        h.write(f"<p>{ntitles} article titles, {len(imgs)} decoded images.</p><div>")
        for p in imgs:
            h.write(f'<img src="images/{html.escape(os.path.basename(p))}" '
                    f'style="margin:3px;max-height:180px">')
        h.write("</div>")
    print(f"[*] wrote {os.path.join(a.out, 'index.html')} ({len(imgs)} images)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
