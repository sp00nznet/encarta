#!/bin/sh
# Run the cross-built Encarta 97 harness under Wine (Linux) or CrossOver (macOS).
# The same job tools/localcontent/run-encarta.ps1 does on Windows: see that
# script for why each variable exists.
#
#   ./play.sh /path/to/encarta97-mirror            # mirror-cd.ps1 output, or the CD
#   ./play.sh /media/cdrom --hold --file-log all
#   ./play.sh /path/to/mirror --headless --record out.mp4   # Linux: Xvfb, no window
#
# The app folder (ENC97.EXE and its DLLs) is analysis/ unless ENCARTA_APP says
# otherwise. On a Mac CX_BOTTLE picks the bottle (default Encarta); WINE another
# wine launcher, anywhere.
set -e
# --headless: the same run on a virtual X server, so nothing opens on a screen.
# Re-run under xvfb-run once; the marker stops it recursing.
case " $* " in *" --headless "*)
  if [ -z "$ENC97_XVFB" ]; then
    command -v xvfb-run >/dev/null || { echo "--headless needs xvfb-run (apt install xvfb)" >&2; exit 1; }
    ENC97_XVFB=1 exec xvfb-run -a -s "-screen 0 1280x1024x24" "$0" "$@"
  fi ;;
esac
REPO=$(cd "$(dirname "$0")" && pwd)
usage() { echo "usage: $0 <content dir> [--hold] [--file-log 1|all] [--no-video] [--timeout-ms N] [--headless] [--record out.mp4]" >&2; exit 2; }
[ -n "$1" ] || usage
CONTENT=$(cd "$1" && pwd); shift
TIMEOUT_MS=900000
while [ $# -gt 0 ]; do
  case "$1" in
    --hold) export HOLD=1 ;;
    --file-log) export FILE_LOG="$2"; shift ;;
    --no-video) export NO_VIDEO=1 ;;
    --timeout-ms) TIMEOUT_MS=$2; shift ;;
    --headless) ;;
    --record) RECORD=$(cd "$(dirname "$2")" && pwd)/$(basename "$2"); shift ;;
    *) usage ;;
  esac
  shift
done

HARNESS=${HARNESS:-$REPO/build-x86/tools/recomp/recomp_enc97_run.exe}
[ -f "$HARNESS" ] || { echo "no $HARNESS: run ./build.sh first" >&2; exit 1; }
APP=$(cd "${ENCARTA_APP:-$REPO/analysis}" 2>/dev/null && pwd) ||
  { echo "no app folder ${ENCARTA_APP:-$REPO/analysis} (set ENCARTA_APP)" >&2; exit 1; }
[ -f "$APP/ENC97.EXE" ] || { echo "no ENC97.EXE in $APP (set ENCARTA_APP)" >&2; exit 1; }
[ -f "$CONTENT/ENCYC97/ENCARTA.M20" ] ||
  { echo "$CONTENT does not look like Encarta's CD1 (no ENCYC97/ENCARTA.M20)" >&2; exit 1; }

# Windows ships MFC40.DLL and MSVCRT40.DLL in System32; Wine does not. CD1 has
# both, with the two picture DLLs, so put them where the harness looks first.
for d in MFC40.DLL MSVCRT40.DLL AM16.DLL AMF16.DLL; do
  [ -f "$APP/$d" ] || cp "$CONTENT/AAMSSTP/SYSTEM32/$d" "$APP/" ||
    echo "warning: no $d on the CD - the app may not start" >&2
done

if [ "$(uname)" = Darwin ]; then
  BOTTLE=${CX_BOTTLE:-Encarta}
  WINE=${WINE:-/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine}
  DOSDEV="$HOME/Library/Application Support/CrossOver/Bottles/$BOTTLE/dosdevices"
  set -- "$WINE" --bottle "$BOTTLE" --workdir "$APP"
else
  export WINEPREFIX=${WINEPREFIX:-$HOME/.wine}
  WINE=${WINE:-wine}
  [ -d "$WINEPREFIX/dosdevices" ] || WINEDEBUG=-all "$WINE" wineboot -i >/dev/null 2>&1
  DOSDEV="$WINEPREFIX/dosdevices"
  set -- "$WINE"
fi

# The subst of the Windows script: the content answers as drive H:, and
# ENC97_CDROM makes that drive report itself as a CD-ROM labelled CD1.
[ -d "$DOSDEV" ] || { echo "no $DOSDEV: create the bottle/prefix first" >&2; exit 1; }
ln -sfn "$CONTENT" "$DOSDEV/h:"
winpath() { echo "Z:$1" | tr / '\\'; }   # Wine maps / as Z: in every prefix
export ENC97_CDROM=H
# Deluxe=#1 and the fonts stand in for what Setup writes and installs; see
# run-encarta.ps1.
export ENC97_PROFILE="CodePath=$(winpath "$APP/");DATPath=$(winpath "$APP/");BookPath=H:\\ENCYC97\\;DiscPath0=H:\\;UpdatesPath=H:\\AAMSSTP\\ENCARTA\\UPDATES\\;Deluxe=#1"
export ENC97_FONTS='H:\AAMSSTP\FONTS'
export MSGBOX_LOG=1 NO_PRINTDLG=1
export IR32_DLL='H:\AAMSSTP\SYSTEM16\IR32.DLL'
# ponytail: the video bridge (ir32vfw.dll) is built by build.bat, MSVC only, so
# a cross build has no video yet; docs/porting.md tracks moving it into CMake.
[ -f "$(dirname "$HARNESS")/ir32vfw.dll" ] || export NO_VIDEO=1

cd "$APP"   # the app keeps its annotations file (encart97.ann) in the working folder
echo "content : $CONTENT (as H:, answering as CD-ROM)"
echo "app     : $APP"
echo "video   : $([ -n "$NO_VIDEO" ] && echo disabled || echo 'IV32 via the recompiled codec')"
[ -n "$RECORD" ] || exec "$@" "$HARNESS" "$(winpath "$APP/ENC97.EXE")" "$TIMEOUT_MS"

# --record: grab the X display while the app runs, and stop ffmpeg with SIGINT
# so it finishes the file. Recording a real desktop works too; it captures the
# whole display, so pair it with --headless.
[ -n "$DISPLAY" ] || { echo "--record needs an X display (add --headless)" >&2; exit 1; }
export HOLD=1
ffmpeg -loglevel error -y -f x11grab -framerate 15 -video_size 1280x1024 \
       -i "$DISPLAY" -pix_fmt yuv420p "$RECORD" &
FF=$!
rc=0; "$@" "$HARNESS" "$(winpath "$APP/ENC97.EXE")" "$TIMEOUT_MS" || rc=$?
kill -INT $FF; wait $FF || true
echo "recorded: $RECORD"
exit $rc
