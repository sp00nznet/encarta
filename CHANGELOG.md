# Changelog

All notable changes to this project. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/). There is no tagged release yet; the
history before this file is in `git log`.

## [Unreleased]

### Added
- **Linux and macOS.** `build.sh` cross-builds the Win32 harness and tools with
  clang-cl against xwin's CRT and SDK (`cmake/clang-cl-x86.cmake`, from
  redalert2-recomp, where @cpressland contributed it). `play.sh` runs it under
  Wine or CrossOver. On Linux under Wine 10 the app boots and renders articles
  and pictures from a fresh prefix; macOS is built in CI but not yet run.
  ([docs/porting.md](docs/porting.md))
- `play.sh --headless --record out.mp4`: Xvfb and ffmpeg, no window.
- The data tools (`m20dump`, `strdump`, `datdump`, `ftcdecode`, `ftcprobe`,
  `indeodec`) build natively with gcc and clang.
- CI (`.github/workflows/ci.yml`): native data tools on Linux and macOS, the
  Win32 tree with MSVC, and the clang-cl cross build on Linux and macOS.
- `tools/regress.py --update` and `conformance.json`: the conformance baseline,
  9 of 9; a check in it that fails is reported as a regression.
- `ENC97_PROFILE` values beginning `#` answer as REG_DWORD; `ENC97_FONTS` loads
  a folder of fonts for the process only; `IAT_LOG` names unresolved imports;
  the exception handler names the C++ type of an MSVC throw.
- `lift.py --exports`: lift what a DLL's exports reach, so no address list has
  to be kept.
- `CONTRIBUTING.md`, `CONTRIBUTORS.md`, `CHANGELOG.md`, `docs/architecture.md`,
  `docs/porting.md`.

### Fixed
- **The app died at startup on current Windows 11.** Real code ran on the
  harness's emulated stack with the TIB still describing the host stack, so x86
  SEH rejected every exception raised there, even ones the code catches itself.
  A Windows update began throwing and catching a `wil::ResultException` inside
  MFC40 at startup, and that was enough. `call_machine` now points the TIB at
  the emulated stacks for each call into real code.
- EEUIL10.DLL is loaded with `LOAD_WITH_ALTERED_SEARCH_PATH`, so its MFC40
  import resolves from the app folder. Under Wine it failed to load, leaving 93
  imports unwired.
- The launchers answer `Deluxe`, `DiscPath0` and `UpdatesPath` and load the CD's
  fonts, which Setup would have written and installed. Without them the 2-CD
  edition asked for the 1-CD edition's `HILITSTD.M20`, or showed "Required
  files are missing". This machine's registry had been supplying them.
- `cpu.h` declares `dispatch`, which clang 16+ rejects implicitly declaring;
  `cpu_rt.c` defines `__writefsdword` for clang-cl, which declares it but has
  no builtin.
- `m20dump` creates directories on POSIX.

### Changed
- `ROADMAP.md` moved to the repository root.
- `LICENSE` is the plain MIT text; what it does not cover is in the README.
- CMake targets that need generated lifts exist only once those lifts do.

### Removed
- Product-derived files still tracked, per the rule that generated source is
  never distributed: the lifted C (`enc97_one.c`, `enc97_one_list.h`,
  `enc97_callable.h`, `enc97_write_leaves.h`), DECO_32's
  function lists (`deco_funcs.txt`, `hot_functions.txt`) and the `encart97.ann`
  stubs. All are made locally and are now gitignored; they remain in earlier
  commits. The `fif_test/` test vectors stay, as a deliberate exception.
