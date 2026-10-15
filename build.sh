#!/bin/sh
# Build Encarta 97's recompiled harness on Linux or macOS.
#
# The harness is a 32-bit Windows program (the recompiled app still calls real
# MFC40 and Win32 underneath - docs/porting.md), so off Windows it is cross-built
# with clang-cl and lld-link against xwin's MSVC CRT and Windows SDK
# (cmake/clang-cl-x86.cmake), then run under Wine or CrossOver (play.sh).
#
#   ./build.sh                       # -> build-x86/tools/recomp/recomp_enc97_run.exe
#   ./build.sh --target m20dump      # any one target
#
# The generated lift (tools/recomp/enc97_full.c) must exist first:
# docs/GETTING-STARTED.md. Without it only the data tools and test_vlc build.
#
# Needs: clang, lld, cmake, ninja and xwin (macOS: brew install llvm lld cmake
# ninja xwin; Debian/Ubuntu: apt install clang lld llvm cmake ninja-build, and
# cargo install xwin), and once:
#        xwin --accept-license --arch x86 splat --output ~/.xwin
set -e
cd "$(dirname "$0")"
BUILD_DIR=${BUILD_DIR:-build-x86}
if [ ! -f "$BUILD_DIR/build.ninja" ]; then
  # shellcheck disable=SC2086  # CMAKE_ARGS is a list of flags
  cmake -S . -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE=cmake/clang-cl-x86.cmake $CMAKE_ARGS
fi
cmake --build "$BUILD_DIR" "$@"
