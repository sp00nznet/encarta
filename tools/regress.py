#!/usr/bin/env python3
"""Check that everything that works still works.

Enough of this project now works that protecting it matters more than the next
feature: a byte-exact Indeo decoder, a VFW bridge the app plays video through,
an application that boots from a hard disk, and article text extraction. Each
was verified by hand, one command at a time, and nothing re-checked them
together.

That is not a theoretical risk. pcrecomp's lifter changed underneath this repo
mid-session and was only noticed because it happened to surface as a link
error; had it changed something subtler, the decoder would have quietly stopped
being byte-exact. This is the script that would have caught it.

    py tools/regress.py              the fast set, a couple of minutes
    py tools/regress.py --full       every frame rather than six
    py tools/regress.py --list       what it would run, and why each is skipped

A check whose inputs are missing is reported as SKIP, never as a pass. The
exit code is nonzero only for real failures, so this is usable in a hook
without a CD in the drive.
"""
import argparse
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

IR32_DLL_CANDIDATES = [
    r"H:\AAMSSTP\SYSTEM16\IR32.DLL",
    r"G:\encarta97\AAMSSTP\SYSTEM16\IR32.DLL",
]
AVI_CANDIDATES = ["H:/ENCYC97/MM/AVI", "G:/encarta97/ENCYC97/MM/AVI"]
# Where ENCARTA.M20 was extracted with `m20dump -x`. ENC97_M20 overrides:
# there is no conventional location, it is wherever you unpacked it.
M20_CANDIDATES = [q for q in [
    os.environ.get("ENC97_M20"),
    os.path.join(REPO, "enc_dir"),
    os.path.join(REPO, "analysis", "enc_dir"),
] if q]


def first_existing(paths):
    for p in paths:
        if os.path.exists(p):
            return p
    return None


class Result:
    def __init__(self, name, status, detail="", secs=0.0):
        self.name, self.status, self.detail, self.secs = name, status, detail, secs


def run(cmd, cwd=None, timeout=900, env=None):
    e = dict(os.environ)
    if env:
        e.update(env)
    p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True,
                       timeout=timeout, env=e)
    return p.returncode, (p.stdout or "") + (p.stderr or "")


# ---- the checks ---------------------------------------------------------
# Each returns (status, detail). "skip" carries the reason, so a missing CD
# reads as "not checked" rather than "fine".

def check_codec_init(a):
    dll = first_existing(IR32_DLL_CANDIDATES)
    exe = os.path.join(REPO, "tools", "indeo", "runtime", "build", "ir32_run.exe")
    if not dll:
        return "skip", "no IR32.DLL (insert CD1 or mirror it)"
    if not os.path.exists(exe):
        return "skip", "ir32_run.exe not built"
    rc, out = run([exe, dll, "init"], timeout=120)
    if "matches the disassembly" in out:
        return "pass", "init matches the disassembly"
    return "fail", out.strip().splitlines()[-1] if out.strip() else "no output"


def _verify(a, script, want, limit_flag=True):
    """Run one of the tools/indeo verifiers and read its final tally."""
    dll = first_existing(IR32_DLL_CANDIDATES)
    avi = first_existing(AVI_CANDIDATES)
    exe = os.path.join(REPO, "tools", "indeo", "runtime", "build", "ir32_run.exe")
    if not dll or not avi:
        return "skip", "no Indeo content (CD or mirror)"
    if not os.path.exists(exe):
        return "skip", "ir32_run.exe not built"
    cmd = [sys.executable, script]
    if limit_flag and not a.full:
        cmd += ["--limit", "6"]
    rc, out = run(cmd, cwd=os.path.join(REPO, "tools", "indeo"))
    m = re.search(r"(\d+) of (\d+) ", out)
    if not m:
        return "fail", (out.strip().splitlines() or ["no output"])[-1]
    good, total = int(m.group(1)), int(m.group(2))
    if total == 0:
        return "fail", "nothing was checked"
    if good == total:
        return "pass", "%d of %d %s" % (good, total, want)
    return "fail", "%d of %d %s" % (good, total, want)


def check_decoder_exact(a):
    return _verify(a, "verify_frames.py", "frames byte-exact")


def check_icm_picture(a):
    return _verify(a, "verify_rgb.py", "frames correct through ICM")


def check_vfw_path(a):
    exe = os.path.join(REPO, "tools", "indeo", "runtime", "build", "ir32_vfwtest.exe")
    if not os.path.exists(exe):
        return "skip", "ir32_vfwtest.exe not built"
    return _verify(a, "verify_vfw.py", "frames through Video for Windows")


def check_phrase_encoding(a):
    d = first_existing(M20_CANDIDATES)
    if not d:
        return "skip", "no extracted M20 (set ENC97_M20, see tools/mvbtext)"
    rc, out = run([sys.executable, "mvbtext.py", d, "check"],
                  cwd=os.path.join(REPO, "tools", "mvbtext"), timeout=300)
    if "all checks passed" in out:
        return "pass", "the four anchor codes and the single-byte form"
    return "fail", (out.strip().splitlines() or ["no output"])[-1]


def check_article_text(a):
    """The Russia and 'A' articles still come out as prose.

    Anchored on the opening words rather than a length or a hash: a change that
    breaks the phrase indexing produces text of about the right size made of
    the wrong words, which only a content check notices.
    """
    d = first_existing(M20_CANDIDATES)
    if not d:
        return "skip", "no extracted M20 (set ENC97_M20, see tools/mvbtext)"
    want = [("_00000000", "first letter and first vowel of the English alphabet"),
            ("_00006060", "independent republic in eastern Europe and Asia")]
    for topic, phrase in want:
        rc, out = run([sys.executable, "mvbtext.py", d, "prose", topic],
                      cwd=os.path.join(REPO, "tools", "mvbtext"), timeout=300)
        if phrase not in out:
            return "fail", "%s does not contain %r" % (topic, phrase[:40])
    return "pass", "%d articles read as prose" % len(want)


def check_media_list(a):
    d = first_existing(M20_CANDIDATES)
    if not d:
        return "skip", "no extracted M20 (set ENC97_M20, see tools/mvbtext)"
    rc, out = run([sys.executable, "mvbtext.py", d, "prose", "_00006060"],
                  cwd=os.path.join(REPO, "tools", "mvbtext"), timeout=300)
    n = len(re.findall(r"^# media ", out, re.M))
    if n >= 20 and "Red Square, Moscow" in out:
        return "pass", "%d media references, captions intact" % n
    return "fail", "%d media references found" % n


def check_app_boots(a):
    """The lifted ENC97 reaches its main window.

    The one check that exercises the whole stack at once - 7,326 lifted
    functions, the 914-import IAT, the content path and the codec bridge - and
    the one a lifter change is most likely to break silently.
    """
    exe = os.path.join(REPO, "build", "tools", "recomp", "Release",
                       "recomp_enc97_run.exe")
    app = os.path.join(REPO, "analysis", "ENC97.EXE")
    if not os.path.exists(exe):
        return "skip", "recomp_enc97_run.exe not built"
    if not os.path.exists(app):
        return "skip", "no analysis/ENC97.EXE"
    content = first_existing(["G:\\encarta97", "H:\\"])
    if not content:
        return "skip", "no content (CD or mirror)"
    appdir = os.path.join(REPO, "analysis") + "\\"
    env = {
        "ENC97_PROFILE": "CodePath=%s;DATPath=%s;BookPath=%s\\ENCYC97\\"
                         % (appdir, appdir, content.rstrip("\\")),
        "ENC97_CDROM": content[0],
        "MSGBOX_LOG": "1",
        "NO_PRINTDLG": "1",
    }
    if content.rstrip("\\").lower() != "h:":
        env["ENC97_REDIRECT"] = "H:\\=%s\\" % content.rstrip("\\")
    try:
        rc, out = run([exe, app, "25000"], timeout=180, env=env,
                      cwd=os.path.dirname(exe))
    except subprocess.TimeoutExpired:
        return "fail", "timed out before the window appeared"
    if "window appeared" in out:
        m = re.search(r"window appeared: (\d+) calls", out)
        return "pass", "main window, %s dispatched calls" % (m.group(1) if m else "?")
    last = [l for l in out.splitlines() if l.strip()][-1:] or ["no output"]
    return "fail", last[0][:70]


def check_differential(a):
    """974 ENC97 functions still match the real originals.

    The one check that compares lifted code against the code it was translated
    from, rather than against an expectation of what it should do.
    """
    exe = os.path.join(REPO, "build", "tools", "recomp", "Release",
                       "recomp_enc97_full.exe")
    app = os.path.join(REPO, "analysis", "ENC97.EXE")
    if not os.path.exists(exe):
        return "skip", "recomp_enc97_full.exe not built (needs enc97_full.c)"
    if not os.path.exists(app):
        return "skip", "no analysis/ENC97.EXE"
    rc, out = run([exe, app], timeout=600)
    got = sum(int(m) for m in re.findall(r"(\d+) matched", out))
    if "ALL PASS" in out:
        return "pass", "%d functions match the originals" % got
    bad = [l.strip() for l in out.splitlines() if "MISMATCH" in l][:1]
    return "fail", bad[0][:60] if bad else "sweep did not pass"


# No check here for the lifter's instruction semantics, deliberately.
#
# difftest.py compares every distinct instruction against Unicorn and is worth
# running when the lifter changes, but wiring it in would mean generating a
# harness, invoking MSVC and building it - and what it would add is already
# covered. Sixty-eight frames decoding to pixels identical to FFmpeg is a
# stronger statement about the lift than any instruction-level check: a wrong
# translation anywhere on that path shows up as wrong pixels.
#
# Run it by hand after touching pcrecomp:
#     py tools/indeo/difftest.py <IR32.DLL> --seg 3
#     py tools/indeo/difftest16.py <IR32.DLL> --seg 7 --out runtime/build


CHECKS = [
    ("codec loads",        check_codec_init),
    ("decoder byte-exact", check_decoder_exact),
    ("ICM returns picture", check_icm_picture),
    ("VFW finds codec",    check_vfw_path),
    ("phrase encoding",    check_phrase_encoding),
    ("article text",       check_article_text),
    ("media list",         check_media_list),
    ("app reaches window", check_app_boots),
    ("974 fns match real", check_differential),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--full", action="store_true",
                    help="every frame rather than a sample of six")
    ap.add_argument("--list", action="store_true", help="list the checks and exit")
    ap.add_argument("-k", metavar="SUBSTR", help="only checks matching SUBSTR")
    a = ap.parse_args()

    checks = [(n, f) for n, f in CHECKS if not a.k or a.k.lower() in n.lower()]
    if a.list:
        for n, f in checks:
            print("  %-20s %s" % (n, (f.__doc__ or "").strip().splitlines()[0]
                                  if f.__doc__ else ""))
        return 0

    print("regression: %s set%s\n" % ("full" if a.full else "fast",
                                      "" if not a.k else " matching %r" % a.k))
    results = []
    for name, fn in checks:
        t0 = time.time()
        sys.stdout.write("  %-20s ... " % name)
        sys.stdout.flush()
        try:
            status, detail = fn(a)
        except Exception as e:                     # a broken check is a failure
            status, detail = "fail", "%s: %s" % (type(e).__name__, e)
        secs = time.time() - t0
        results.append(Result(name, status, detail, secs))
        mark = {"pass": "ok  ", "fail": "FAIL", "skip": "skip"}[status]
        print("%s  %-52s %5.1fs" % (mark, detail[:52], secs))

    npass = sum(r.status == "pass" for r in results)
    nfail = sum(r.status == "fail" for r in results)
    nskip = sum(r.status == "skip" for r in results)
    print("\n%d passed, %d failed, %d skipped" % (npass, nfail, nskip))
    if nskip:
        print("skipped checks are NOT passes - their inputs were missing:")
        for r in results:
            if r.status == "skip":
                print("   %-20s %s" % (r.name, r.detail))
    if nfail:
        print("\nFAILED:")
        for r in results:
            if r.status == "fail":
                print("   %-20s %s" % (r.name, r.detail))
    return 1 if nfail else 0


if __name__ == "__main__":
    sys.exit(main())
