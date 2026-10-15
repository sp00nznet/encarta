# Linux and macOS

How Encarta 97 runs off Windows today, what it took, and what a native port
still needs. Measured on 2026-10-14.

## What runs

The recompiled harness is a 32-bit Windows program. Off Windows it is
**cross-built** with clang-cl and lld-link against xwin's MSVC CRT and Windows
SDK, and **run under Wine** (Linux) or **CrossOver** (macOS):

```
./build.sh                                   # cmake/clang-cl-x86.cmake, Ninja
./play.sh /path/to/cd1-mirror                # Wine or CrossOver
```

On Debian 13 with Wine 10.0, under Xvfb, from a fresh Wine prefix with nothing
installed: the lifted ENC97 boots, all 914 imports wire, the title screen comes
up clean, and **encyclopedia articles** opens article "A" with its text, its
fractal-decoded picture and the Pinpointer listing 31,108 articles
([screenshot](encarta97-linux.png)). The same `recomp_enc97_run.exe`, built on a
Linux clang-cl builder, passes all 9 checks of `tools/regress.py` on Windows 11.

| | Linux (Wine 10) | macOS (CrossOver) | Windows 11 |
|---|---|---|---|
| Cross-build from this OS | yes (netlab clang-cl builder) | same toolchain as redalert2-recomp; **not run here** | n/a (MSVC or clang-cl) |
| App boots, articles render | **yes** | **untested** | yes |
| Video | no (see below) | no | yes |
| Data tools (`m20dump`, `ftcdecode`, ...) native | yes, gcc | expected (plain C); CI builds them | yes |

macOS has not been run: there is no Mac in the lab. The toolchain file and the
CrossOver launch line are the ones redalert2-recomp uses on Apple Silicon, so
the build is expected to work; whether the harness's stack switch survives
CrossOver's 32-on-64 mode is the open question. Report it if you try.

## What it took

Three things stopped the first Wine run. Two of them were already broken on
Windows and hidden by state this one machine had kept.

**1. Real code could not catch its own exceptions.** The harness runs real DLL
code (MFC40, user32, ...) with `esp` on its emulated stack, a `VirtualAlloc`
block. x86 SEH only accepts an exception registration record that lies between
the TIB's `StackLimit` (`fs:[8]`) and `StackBase` (`fs:[4]`), so any exception
raised there was unhandled, including the ones the real code catches itself.
That was latent until a Windows 11 update started throwing, and catching, a
`wil::ResultException` (HRESULT `0x8007007E`) inside MFC40 ordinal 1368 at
startup:

```
VEH fault 0xE06D7363 @76591E54 (external) after 1045 calls; last lifted=0x537AB0 last import=0x5F80D26D (MFC40.DLL#1368)
  C++ throw of .?AVResultException@wil@@
  object: 74CD27DC 00000000 00000000 00000000 00000000 8007007E
```

`call_machine` now points the TIB at the emulated stacks (both are one
allocation) for each call into real code, and puts the host's bounds back
after, so `__chkstk` on the host stack still probes correctly. The handler now
names the C++ type of any MSVC throw; that line is what found it.

**2. EEUIL10 did not load.** `IAT 821/914 wired`. Under Wine, with
`IAT_LOG=1`:

```
err:module:import_dll Library MFC40.DLL (which is needed by L"Z:\\...\\app\\EEUIL10.dll") not found
```

Windows ships MFC40 in SysWOW64, so the dependency always resolved there. Wine
has none, and a plain `LoadLibrary` does not search the DLL's own folder for its
imports. `load_for` now uses `LOAD_WITH_ALTERED_SEARCH_PATH`, and `play.sh`
copies MFC40.DLL and MSVCRT40.DLL from CD1's `AAMSSTP\SYSTEM32` beside ENC97.

**3. Values Setup writes.** A fresh prefix has none of them, and the app fails
in three different ways without them. Each is now answered through
`ENC97_PROFILE`, which gained `#N` for a REG_DWORD:

| Value | Without it |
|---|---|
| `Deluxe=#1` | the 2-CD edition opens the 1-CD edition's `HILITSTD.M20`, which is not on the disc |
| `DiscPath0=H:\` | the app looks for `ENC97.CD1` on D: and shows "Required files are missing" |
| `UpdatesPath=H:\AAMSSTP\ENCARTA\UPDATES\` | `artlnk.ewl` is opened relative to the working folder; same dialog |

The fonts Setup installs (`MSREF1.TTF`, `MSREF2.TTF`, `PHONETIC.FON`, from
`AAMSSTP\FONTS`) are loaded for the process only (`ENC97_FONTS`,
`AddFontResourceEx(..., FR_PRIVATE)`), so nothing is installed on the host.

## What is not there yet

- **Video.** The bridge (`ir32vfw.dll`) is built by `tools/indeo/runtime/build.bat`,
  which is MSVC-only and not a CMake target, so a cross build has no codec and
  `play.sh` sets `NO_VIDEO=1`. Whether Wine's MCIAVI and ICM accept a codec
  installed with `ICInstall` is untested.
- **A native port.** Running under Wine still runs Microsoft's MFC40, MSVCRT40
  and EEUIL10, on Wine's Win32. A native Linux or macOS binary means
  recompiling those three as well and answering what they import from
  [pcrecomp](https://github.com/sp00nznet/pcrecomp)'s `win32hle` (the route
  redalert2-recomp's native Linux build takes, as a 32-bit `-m32` process). The
  steps are in [ROADMAP.md](../ROADMAP.md).
