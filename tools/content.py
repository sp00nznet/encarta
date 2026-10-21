#!/usr/bin/env python3
r"""Copy your Encarta 97 CD1 into this checkout, from wherever it is.

    py -3 tools/content.py E:\                     # the CD in a drive
    py -3 tools/content.py D:\encarta97            # a copy of the CD (mirror-cd.ps1's, say)
    py -3 tools/content.py CD1ENC97ENC.iso         # an image of it, or a folder holding one

It makes two folders, both gitignored:

    content/    the whole of CD1 (about 645 MB), so no disc is needed to run
    analysis/   the app: AAMSSTP/ENCARTA/* and the two picture DLLs Setup puts
                in System32 (AM16.DLL, AMF16.DLL)

Names are written in upper case, as on the disc, whatever case a mount shows.
An image is read with pcrecomp's ISO9660 walker (tools/assets/iso_peek.py), so
nothing is mounted. Your own copy, kept on your machine; nothing is downloaded.
"""
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CONTENT = os.path.join(ROOT, "content")
APP = os.path.join(ROOT, "analysis")
MARK = "ENCYC97/ENCARTA.M20"     # what makes a disc CD1


def norm(rel):
    return rel.replace("\\", "/").upper()


def from_tree(src):
    for d, _, files in os.walk(src):
        for f in files:
            p = os.path.join(d, f)
            yield norm(os.path.relpath(p, src)), os.path.getsize(p), lambda p=p: open(p, "rb")


def from_iso(path):
    sys.path.insert(0, os.path.join(os.environ.get("PCRECOMP", os.path.join(ROOT, "..", "pcrecomp")),
                                    "tools", "assets"))
    import iso_peek

    class Slice:
        """One file of the image, read in place."""
        def __init__(self, off, size):
            self.fh, self.left = open(path, "rb"), size
            self.fh.seek(off)
        def read(self, n=-1):
            n = self.left if n < 0 else min(n, self.left)
            self.left -= n
            return self.fh.read(n)
        def __enter__(self):
            return self
        def __exit__(self, *a):
            self.fh.close()

    for rel, off, size in iso_peek.walk(iso_peek.Reader(path)):
        yield norm(rel), size, lambda off=off, size=size: Slice(off, size)


def source(where):
    """The CD1 file list from a folder, a drive, an .iso, or a folder holding one."""
    if not os.path.exists(where):
        sys.exit("%s: no such file or folder" % where)
    if os.path.isfile(where):
        return list(from_iso(where))
    if os.path.isfile(os.path.join(where, *MARK.split("/"))) or \
            os.path.isfile(os.path.join(where, *MARK.lower().split("/"))):
        return list(from_tree(where))
    for f in sorted(os.listdir(where)):
        if f.lower().endswith(".iso"):
            files = list(from_iso(os.path.join(where, f)))
            if any(r == MARK for r, _, _ in files):
                print("using %s" % f)
                return files
    return []


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    files = source(sys.argv[1])
    if not any(r == MARK for r, _, _ in files):
        sys.exit("%s is not Encarta 97's CD1 (no %s). Give the CD, a copy of it, or its .iso."
                 % (sys.argv[1], MARK))
    total = sum(s for _, s, _ in files)
    print("copying %d files (%d MB) into content/" % (len(files), total >> 20))
    for rel, size, opener in files:
        dst = os.path.join(CONTENT, *rel.split("/"))
        if os.path.isfile(dst) and os.path.getsize(dst) == size:
            continue                       # a rerun resumes
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with opener() as i, open(dst + ".part", "wb") as o:
            shutil.copyfileobj(i, o, 1 << 20)
        os.replace(dst + ".part", dst)

    os.makedirs(APP, exist_ok=True)
    app = [os.path.join(CONTENT, "AAMSSTP", "ENCARTA", f)
           for f in os.listdir(os.path.join(CONTENT, "AAMSSTP", "ENCARTA"))]
    app += [os.path.join(CONTENT, "AAMSSTP", "SYSTEM32", f) for f in ("AM16.DLL", "AMF16.DLL")]
    for p in app:
        if os.path.isfile(p):
            shutil.copy2(p, APP)
    if not os.path.isfile(os.path.join(APP, "ENC97.EXE")):
        sys.exit("no AAMSSTP/ENCARTA/ENC97.EXE on this disc")
    print("app in analysis/, CD1 in content/")


if __name__ == "__main__":
    main()
