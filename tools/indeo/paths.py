"""Where the Indeo inputs are: the 16-bit driver, the clips, and ffmpeg.

The three verifiers used to hardcode `H:` and one machine's WinGet ffmpeg
directory. That is not a portability nicety - it made the checks *lie*. With
the disc mirrored to G: instead, the AVI glob matched nothing and every
verifier printed "0 of 0", which regress.py reported as a failure with nothing
behind it. A check that cannot find its inputs must say so.

Override with ENC97_IR32 / ENC97_AVI / ENC97_FFDIR; otherwise the same
candidates regress.py uses, and ffmpeg from PATH.
"""
import os
import shutil

_IR32 = [r"H:\AAMSSTP\SYSTEM16\IR32.DLL",
         r"G:\encarta97\AAMSSTP\SYSTEM16\IR32.DLL"]
_AVI = ["H:/ENCYC97/MM/AVI", "G:/encarta97/ENCYC97/MM/AVI"]


def _first(env, candidates):
    v = os.environ.get(env)
    if v:
        return v
    for p in candidates:
        if os.path.exists(p):
            return p
    return candidates[0]          # report the miss where it is used


DLL = _first("ENC97_IR32", _IR32)
AVIDIR = _first("ENC97_AVI", _AVI)
FFDIR = os.environ.get("ENC97_FFDIR") or (
    os.path.dirname(shutil.which("ffprobe") or "") or ".")


def require(avidir=None):
    """Fail loudly rather than quietly measuring nothing."""
    avidir = avidir or AVIDIR
    missing = []
    if not os.path.exists(DLL):
        missing.append("IR32.DLL (set ENC97_IR32): %s" % DLL)
    if not os.path.isdir(avidir):
        missing.append("AVI directory (set ENC97_AVI): %s" % avidir)
    if not shutil.which(os.path.join(FFDIR, "ffprobe")):
        missing.append("ffprobe (set ENC97_FFDIR or put ffmpeg on PATH)")
    if missing:
        raise SystemExit("cannot verify, inputs missing:\n  " +
                         "\n  ".join(missing))
