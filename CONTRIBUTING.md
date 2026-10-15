# Contributing

Issues and pull requests are welcome.

## The one rule

**Nothing from the product goes in the repository.** No Encarta files (EXE,
DLLs, `.M20`, `.AVI`, fonts), no lifted C (`tools/recomp/enc97_*`,
`lifted_*`, `encapi_lifted*`, `tools/indeo/runtime/build/`), no function maps,
disassembly or traces of the original DLLs, and no fragments cut out of the
containers as "test vectors" (`fif_test/` is the one existing exception; don't
add to it). The tool ships; its output is made on each
person's machine from their own copy. `.gitignore` covers the usual paths;
check `git status` before you commit anyway. Screenshots of it running are fine.

## Where your code comes from

Contributions must be your own work or MIT-compatible. The live risk here is
porting something you saw in a GPL or LGPL project, which relicenses it by
accident and is very hard to untangle later. The projects you are most likely
to have open for this one:

- **Wine** and **ReactOS** (LGPL / GPL) - reimplementations of MFC, Win32, VfW
  and MCI. Running the harness *under* Wine is fine; copying from Wine's source
  is not.
- **FFmpeg / libav** (LGPL, GPL with some options) - its Indeo 3 decoder. FFmpeg
  is used here only as an external program that emits reference frames.
- **DOSBox**, **ScummVM**, **QEMU**, **Unicorn** (GPL).

Reading documentation, specs and wikis, measuring the program itself, and
comparing against a GPL program run as a black box are all fine. If a fix is
unusually polished, expect to be asked where it came from.

## Before a pull request

1. Build: `cmake -B build -G "Visual Studio 17 2022" -A Win32` and
   `cmake --build build --config Release` on Windows, or `./build.sh` on Linux
   and macOS.
2. Run the conformance harness: `py tools/regress.py` (`python3` elsewhere).
   No check may go from pass to fail. Checks whose inputs you lack report SKIP;
   say which ran in the PR.
3. A new behaviour comes with the check that shows it.

## Where a fix belongs

- A bug in the 16-bit lifter, the NE loader or anything else under
  [pcrecomp](https://github.com/sp00nznet/pcrecomp) belongs there, not here.
- A wall you got past goes in the relevant doc: what it looked like, what it
  was, what moved it.
