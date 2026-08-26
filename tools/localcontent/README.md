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
nothing; the log still showed every book being read from `H:`. What works is
rewriting the prefix on the file opens themselves, which is `ENC97_REDIRECT`.
It applies to `CreateFileA`, `OpenFile`, `_lopen` and `FindFirstFileA` - the
four the app uses - and only to paths that start with the prefix.

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

With the mirror in place, a full startup does **26 file operations on the local
copy and none at all on the CD drive**. Measured, not assumed: `-FileLog all`
logs every open and the redirect prints each rewrite, so the two can be counted
separately.

Video works from the mirror too - `IR32.DLL` is at `AAMSSTP\SYSTEM16\` on the
disc and the run script points the codec bridge at the copied one.

## Two things this does not do

The app still *believes* it is reading `H:`, because that is the drive letter it
worked out; the redirect changes where those opens land, not what it thinks.
That means **the CD-absent case is untested** - with the disc ejected the app
would have to arrive at some drive letter on its own, and it was not possible
to check that with the disc mounted. If it picks nothing, `subst`ing a letter
to the mirror should give it one, and `ENC97_CDROM` will make that letter
answer correctly.

`AM16.DLL` and `AMF16.DLL` still have to sit beside `ENC97.EXE` rather than in
the mirror, because `ENCTITLE.DLL` imports them statically and the loader
resolves that before any of this runs. They are on CD1 at `AAMSSTP\SYSTEM32\`.
The run script warns if they are missing.
