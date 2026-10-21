#!/bin/bash
# Linux setup: the same steps as Setup.cmd. The harness is cross-compiled here
# (clang-cl, with xwin's MSVC CRT and Windows SDK) and run under Wine, as it
# runs on Windows. No video yet off Windows (docs/porting.md).
#
#   ./setup.sh                   # asks before installing anything
#   ./setup.sh --yes             # answers yes to every question (recomp-launcher)
#   ./setup.sh --game-dir DIR    # the mounted CD1, a copy of it, its .iso, or a folder holding the .iso
#   ./setup.sh --force           # redo every step
#
# It leaves "Encarta 97.sh" here: run it to start Encarta. A rerun skips
# finished steps, and what each step prints goes to setup.log.
set -e
cd "$(dirname "$0")"
ROOT=$PWD
FORCE=0; YES=0; GAME=""
while [ $# -gt 0 ]; do
  case "$1" in
    --force) FORCE=1 ;;
    --yes) YES=1 ;;
    --game-dir|--game) GAME=$2; shift ;;
    *) echo "usage: ./setup.sh [--yes] [--force] [--game-dir DIR]" >&2; exit 2 ;;
  esac
  shift
done
LOG=$ROOT/setup.log
echo "==== setup $(date -Is)" >> "$LOG"

N=6
say()  { printf '%s\n' "$*" | tee -a "$LOG"; }
# "[n/N] what": recomp-launcher's progress bar reads these, as Setup.cmd's.
STEP=0
step() { STEP=$((STEP + 1)); printf '\n\033[36m[%d/%d] %s\033[0m\n' "$STEP" "$N" "$*"; echo "[$STEP/$N] $*" >> "$LOG"; }
fail() { printf '\n\033[31mSetup stopped: %s\033[0m\n' "$*" >&2; echo "Setup stopped: $*" >> "$LOG"; exit 1; }
ask()  { [ "$YES" = 1 ] && { say "$1 yes"; return 0; }; read -r -p "$1 [Y/n] " r; [[ ! $r =~ ^[nN] ]]; }
done_already() { [ "$FORCE" = 0 ] && [ -e "$1" ]; }
# A program on PATH or, as Debian and Ubuntu keep clang-cl, in /usr/lib/llvm-*/bin.
have() { command -v "$1" >/dev/null || ls /usr/lib/llvm-*/bin/"$1" >/dev/null 2>&1; }
run()  { say "  $1..."; shift; "$@" >> "$LOG" 2>&1 || fail "that failed: the details are in $LOG."; }

say "Encarta 97: static recompilation setup (Linux, under Wine)"
say "You need your own Encarta 97 Encyclopedia CD1 (the disc, a copy of it, or its .iso) and about 3 GB free."

# ---------------------------------------------------------------- tools
step "Checking the tools"
# Encarta opens its files with OpenFile, which takes 127 characters at most
# (under Wine the path is Z:\ and this folder).
longest="Z:$ROOT/analysis/ENCART97.DAT"
[ ${#longest} -le 127 ] || fail "this folder's path is too long for Encarta (${#longest} characters to its data file, 127 at most). Move it somewhere shorter, such as ~/encarta."
need=()
for t in clang-cl lld-link llvm-lib cmake ninja wine python3 curl; do have "$t" || need+=("$t"); done
if [ ${#need[@]} -gt 0 ]; then
  say "  Missing: ${need[*]}. From your package manager, for example:"
  say "    Debian, Ubuntu: sudo apt install clang clang-tools lld llvm cmake ninja-build wine python3-venv curl git"
  say "                    (and for 32-bit Wine: sudo dpkg --add-architecture i386 && sudo apt update && sudo apt install wine32:i386)"
  say "    Fedora:         sudo dnf install clang lld llvm cmake ninja-build wine python3 curl git"
  say "    Arch:           sudo pacman -S clang lld llvm cmake ninja wine python curl git"
  fail "install ${need[*]}, then run ./setup.sh again."
fi
python3 -c 'import sys; sys.exit(sys.version_info < (3, 10))' || fail "Python 3.10 or newer is needed."
# Debian and Ubuntu split 32-bit Wine out; without it setup succeeds and Encarta does not start.
if command -v dpkg >/dev/null && dpkg -s wine >/dev/null 2>&1 && ! dpkg -s wine32:i386 >/dev/null 2>&1; then
  say "  32-bit Wine is missing: sudo dpkg --add-architecture i386 && sudo apt update && sudo apt install wine32:i386"
  fail "install wine32:i386, then run ./setup.sh again."
fi
# Debian and Ubuntu keep clang-cl and friends off PATH
for d in /usr/lib/llvm-*/bin; do [ -x "$d/clang-cl" ] && PATH=$PATH:$d; done

if ! command -v xwin >/dev/null && [ ! -x "$HOME/.local/bin/xwin" ]; then
  say "  xwin (https://github.com/Jake-Shadle/xwin) fetches the MSVC C runtime and Windows SDK."
  ask "  Download its release binary into ~/.local/bin (about 5 MB)?" || fail "xwin is required (or: cargo install xwin)."
  mkdir -p "$HOME/.local/bin"
  v=0.10.0
  curl -fsSL "https://github.com/Jake-Shadle/xwin/releases/download/$v/xwin-$v-x86_64-unknown-linux-musl.tar.gz" |
    tar -xz -C "$HOME/.local/bin" --strip-components=1 "xwin-$v-x86_64-unknown-linux-musl/xwin" ||
    fail "xwin did not download."
fi
PATH=$PATH:$HOME/.local/bin
XWIN_DIR=${XWIN_DIR:-$HOME/.xwin}
if [ ! -d "$XWIN_DIR/crt/lib/x86" ]; then
  say "  The x86 MSVC C runtime and Windows SDK are needed to build the harness (about 1 GB, 10 to 30 minutes, with no progress shown)."
  say "  xwin downloads them from Microsoft, under Microsoft's licence: https://go.microsoft.com/fwlink/?LinkId=2086102"
  ask "  Accept that licence and download them into $XWIN_DIR?" || fail "the MSVC C runtime and Windows SDK are required."
  # The cache beside the output: moving out of /tmp fails where it is tmpfs.
  run "Downloading the C runtime and SDK" \
    xwin --accept-license --cache-dir "$XWIN_DIR.cache" --arch x86 splat --output "$XWIN_DIR"
  rm -rf "$XWIN_DIR.cache"
fi
export XWIN_DIR
say "  C compiler: clang-cl, with $XWIN_DIR"

if python3 -c 'import pefile, capstone' 2>/dev/null; then
  PY=$(command -v python3)
else
  if [ ! -x .venv/bin/python ] || ! .venv/bin/python -c 'import pefile, capstone' 2>/dev/null; then
    say "  The Python packages pefile and capstone are missing (about 20 MB)."
    ask "  Install them into .venv here?" || fail "pefile and capstone are required."
    say "  Installing pefile and capstone..."
    { python3 -m venv .venv && .venv/bin/pip install -q pefile capstone; } >> "$LOG" 2>&1 ||
      fail "Python could not make its venv: install python3-venv (Debian, Ubuntu), then run ./setup.sh again."
  fi
  PY=$ROOT/.venv/bin/python
fi
say "  Python: $PY"

# The toolkit (only its ISO reader is used here): PCRECOMP, else ../pcrecomp.
PCRECOMP=${PCRECOMP:-$(cd .. && pwd)/pcrecomp}
if [ ! -f "$PCRECOMP/tools/assets/iso_peek.py" ]; then
  have git || fail "git is needed to fetch pcrecomp: install it from your package manager."
  ask "  Download the pcrecomp toolkit into $PCRECOMP (git, about 20 MB)?" || fail "pcrecomp is required at $PCRECOMP."
  run "Cloning pcrecomp" git clone --depth 1 https://github.com/sp00nznet/pcrecomp "$PCRECOMP"
fi
export PCRECOMP
say "  pcrecomp: $PCRECOMP"

# ---------------------------------------------------------------- the disc
step "Copying your CD1 into content (about 645 MB)"
if done_already content/ENCYC97/ENCARTA.M20 && [ -e analysis/ENC97.EXE ]; then say "  Already there (skipping)."; else
  if [ -z "$GAME" ]; then
    for m in /media/"$USER" /run/media/"$USER" /mnt; do
      [ -d "$m" ] || continue
      f=$(find "$m" -maxdepth 3 -ipath '*/encyc97/encarta.m20' -print -quit 2>/dev/null)
      [ -n "$f" ] && { GAME=$(dirname "$(dirname "$f")"); break; }
    done
  fi
  if [ -z "$GAME" ]; then
    for d in "$ROOT" "$ROOT/.." "$HOME/Downloads"; do
      ls "$d"/*.iso "$d"/*.ISO >/dev/null 2>&1 && { GAME=$d; break; }
    done
  fi
  [ -n "$GAME" ] && say "  Looking in $GAME"
  [ -z "$GAME" ] && [ "$YES" = 1 ] && fail "Encarta 97's CD1 wasn't found. Give the mounted CD, a copy of it or its .iso with --game-dir."
  while [ -z "$GAME" ]; do
    read -r -p "  Where is Encarta 97's CD1? The mounted disc, a copy of it, or its .iso: " GAME
  done
  run "Copying the disc" "$PY" tools/content.py "$GAME"
fi

# ---------------------------------------------------------------- function map
step "Finding every function in ENC97.EXE (under a minute)"
if done_already tools/recomp/funcs.txt; then say "  Already done (skipping)."
else run "Disassembling" "$PY" tools/recomp/funcmap.py analysis/ENC97.EXE tools/recomp/funcs.txt; fi

# ---------------------------------------------------------------- lift
step "Recompiling ENC97.EXE to C (about a minute)"
if done_already tools/recomp/enc97_full.c; then say "  Already done (skipping)."
else run "Lifting" "$PY" tools/recomp/lift.py analysis/ENC97.EXE tools/recomp/funcs.txt tools/recomp/enc97_full.c; fi

# ---------------------------------------------------------------- build
step "Building the app (a few minutes)"
if done_already build-x86/tools/recomp/recomp_enc97_run.exe; then say "  Already built (skipping)."; else
  [ "$FORCE" = 1 ] && rm -rf build-x86
  run "Compiling" sh build.sh --target recomp_enc97_run
fi

# ---------------------------------------------------------------- launcher
step "Making the launcher"
cat > 'Encarta 97.sh' <<'SH'
#!/bin/sh
# Runs the recompiled Encarta 97 under Wine, from content/ (made by setup.sh).
# WINE picks another wine, WINEPREFIX another prefix. mscoree and mshtml are
# off so a new Wine prefix doesn't stop to offer Mono and Gecko.
cd "$(dirname "$0")" || exit 1
export WINEDLLOVERRIDES=${WINEDLLOVERRIDES:-mscoree,mshtml=}
exec sh ./play.sh content --hold --timeout-ms 2147483647 "$@"
SH
chmod +x 'Encarta 97.sh'
say "  $ROOT/Encarta 97.sh"
printf '\n\033[32mDone.\033[0m Run "Encarta 97.sh" here to start it.\n'
