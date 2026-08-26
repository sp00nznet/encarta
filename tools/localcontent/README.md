# Running Encarta 97 from a hard disk

Two scripts: mirror the CD once, then run from the copy. After the first step
the disc can go back in its case.

```powershell
cd tools\localcontent
.\mirror-cd.ps1  -Source H:\ -Dest G:\encarta97     # ~645 MB, once
.\run-encarta.ps1 -Content G:\encarta97 -Hold
```

`-Hold` keeps the app open so you can click around. Add `-FileLog all` to watch
every file it touches, which is how the numbers below were measured.

## Why it needs more than copying the files

Three separate things assume a disc, and each needed its own answer.

**The app never asks where its content is.** There is a `BookPath` profile
value, and it is not consulted at startup - the app works out a drive letter
itself and opens absolute paths on it. Pointing `BookPath` at a copy changes
nothing; the log still showed every book being read from `H:`.

So the mirror is mapped onto the letter the app asks for, with `subst`. That is
a per-user drive mapping - nothing is mounted, no disc is emulated, and the
machine is not modified - and the run script prints how to undo it
(`subst H: /d`).

The first version did not do this. It rewrote the prefix on the app's own file
opens instead (`ENC97_REDIRECT`, still there behind `-NoSubst`), hooking
`CreateFileA`, `OpenFile`, `_lopen` and `FindFirstFileA`. Every article and
picture loaded. **No video played**, and the reason is the limit of that whole
approach: the app does not open the AVI. It hands the filename to MCI, MCI
loads MCIAVI, and MCIAVI calls its own `CreateFileA` - in its own module, whose
imports we never touched. A patch applied inside one module reaches only what
that module does. A drive letter is resolved by the whole process, which is why
`subst` fixes what the redirect structurally could not.

**It checks the drive is a CD-ROM.** `ENC97.EXE` imports
`GetVolumeInformationA`, `GetLogicalDrives` and `GetDriveTypeA`;
`ENCTITLE.DLL` and `ENCAPI32.DLL` import them too. `ENC97_CDROM=G` makes one
drive letter answer `DRIVE_CDROM` with the label `CD1ENC97ENC`, which is what
CD1 actually carries. Scoped to a single drive letter inside a single process:
nothing is mounted, no drive is emulated, and the machine is not modified.

**It probes for a marker file.** `ENC97.CD1` is zero bytes at the root of the
disc and the app opens it repeatedly to confirm the disc is still there. That
is why `mirror-cd.ps1` copies the whole CD root rather than just `ENCYC97` -
copy only the content folder and the marker is missing, so a complete set of
books still reads as "no disc".

## What it is worth

With the mirror in place, a full startup does **26 file operations, none of
them on a CD drive**. Measured with `-FileLog all`, which logs every open with
its full path: 22 land on the mirror, 3 in the folder holding `ENC97.EXE`, and
one is `encart97.ann` opened by relative path - the annotations file, which the
app creates beside itself.

Video works from the mirror too - `IR32.DLL` is at `AAMSSTP\SYSTEM16\` on the
disc and the run script points the codec bridge at the copied one. It needs the
`subst` mapping, for the reason above; with `-NoSubst` the articles still load
and the videos do not.

## With the disc ejected

It runs. Same 26 operations on the mirror, none anywhere else, window up.

The reason is worth stating, because it is not the one expected. The app does
not go looking for a drive: it asks for `H:\ENC97.CD1` and `H:\ENCYC97\...`
unconditionally, whatever is or is not mounted. That letter is now the mirror,
so **whether a real drive exists never comes up** - with no CD-ROM in the
machine at all, every one of those opens lands on the local copy.

So `-CdDrive` is not "the drive your CD is in", it is "the drive letter this
copy of the app asks for". If yours asks for a different one - because it was
installed against a different drive - set `-CdDrive` to that and the mirror
answers for it just the same.

## One thing to know

`AM16.DLL` and `AMF16.DLL` still have to sit beside `ENC97.EXE` rather than in
the mirror, because `ENCTITLE.DLL` imports them statically and the loader
resolves that before any of this runs. They are on CD1 at `AAMSSTP\SYSTEM32\`.
The run script warns if they are missing.
