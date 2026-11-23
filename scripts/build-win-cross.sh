#!/bin/bash
# PixelPress HAP 1.2.1: builds the Windows pphap.exe on Linux (or a Mac with Homebrew), no Windows PC
# or Visual Studio needed. Output is a single static pphap.exe (no VC++ runtime DLLs).
#
#   bash scripts/build-win-cross.sh [OUTDIR]            (default OUTDIR: dist/win-cross)
#
# How: MinGW-w64 compiles the C++; Intel's Windows ispc.exe runs under Wine to compile the bc7e
# (Hap R) encoder to Windows objects (the Linux/Mac ISPC can't emit them); FFmpeg 4.4 (QuickTime
# writer only, LGPL) is built with MinGW in its own folder. Verified: Hap / Hap Q / Hap R output is
# byte-identical to the MSVC build (scripts/build-win.ps1).
#
# Needs: x86_64-w64-mingw32-gcc-posix/g++-posix (Debian/Ubuntu: mingw-w64; Mac: brew install mingw-w64),
#        wine (wine64), cmake, make, git, curl, unzip. The Adobe SDK is NOT needed (pphap only).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$(mkdir -p "${1:-$ROOT/dist/win-cross}" && cd "${1:-$ROOT/dist/win-cross}" && pwd)"
WORK="${PP_WINX_WORK:-$ROOT/.build-win-cross}"
ISPCV=1.25.3
HOST=x86_64-w64-mingw32
JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
CC=$HOST-gcc-posix; CXX=$HOST-g++-posix
command -v $CC >/dev/null || { CC=$HOST-gcc; CXX=$HOST-g++; }   # Homebrew's mingw-w64 is posix-threads already
for t in $CC $CXX cmake make git curl unzip; do command -v "$t" >/dev/null || { echo "missing $t"; exit 1; }; done
WINE=$(command -v wine64 || command -v wine || true); [ -n "$WINE" ] || { echo "missing wine"; exit 1; }
mkdir -p "$WORK"

# --- sources not stored in the repo ------------------------------------------
bash "$ROOT/scripts/fetch-deps.sh"
SW="$ROOT/external/foundation/external/StackWalker"   # only its folder must exist (cmake/mingw/StackWalker.h stands in)
[ -e "$SW/.fetched" ] || { rm -rf "$SW"; git clone -q --depth 1 https://github.com/JochenKalmbach/StackWalker.git "$SW"; touch "$SW/.fetched"; }

# --- FFmpeg 4.4 static libs for MinGW (own copy; the in-tree one may be a native build) ---
FF="$WORK/ffmpeg"
if [ ! -f "$FF/libavformat/libavformat.a" ]; then
  echo "Building FFmpeg for Windows (one-off) ..."
  rm -rf "$FF"; cp -R "$ROOT/external/foundation/external/ffmpeg/FFmpeg" "$FF"
  (cd "$FF" && (make distclean >/dev/null 2>&1 || true) &&
   ./configure --target-os=mingw32 --arch=x86_64 --cross-prefix=$HOST- --cc=$CC --disable-asm \
     --disable-network --disable-everything --enable-muxer=mov --enable-demuxer=mov --disable-zlib \
     --disable-iconv --disable-programs --disable-doc --disable-autodetect --enable-static --disable-shared >/dev/null &&
   make -j"$JOBS" >/dev/null)
fi

# --- Windows ISPC, run through Wine -------------------------------------------
ISPCDIR="$WORK/ispc-v$ISPCV-windows"
if [ ! -f "$ISPCDIR/bin/ispc.exe" ]; then
  echo "Downloading ISPC $ISPCV (Windows) ..."
  curl -fsSL --retry 3 -o "$WORK/ispc.zip" "https://github.com/ispc/ispc/releases/download/v$ISPCV/ispc-v$ISPCV-windows.zip"
  (cd "$WORK" && unzip -qo ispc.zip && rm ispc.zip)
fi
cat > "$WORK/ispc" <<WRAP
#!/bin/sh
export WINEDEBUG=-all
exec "$WINE" "$ISPCDIR/bin/ispc.exe" "\$@"
WRAP
chmod +x "$WORK/ispc"

# --- toolchain + build ----------------------------------------------------------
cat > "$WORK/toolchain.cmake" <<TC
include("$ROOT/cmake/mingw-w64-toolchain.cmake")
set(CMAKE_C_COMPILER $CC)
set(CMAKE_CXX_COMPILER $CXX)
set(CMAKE_ASM_COMPILER $CC)
TC
cmake -S "$ROOT" -B "$WORK/build" -DCMAKE_TOOLCHAIN_FILE="$WORK/toolchain.cmake" -DCMAKE_BUILD_TYPE=Release \
      -DISPC_EXECUTABLE="$WORK/ispc" -DPP_FFMPEG_DIR="$FF" >/dev/null
cmake --build "$WORK/build" --target pphap -j"$JOBS"
cp "$WORK/build/tools/cli/pphap.exe" "$OUT/pphap.exe"
$HOST-strip "$OUT/pphap.exe"
echo "✓ $OUT/pphap.exe"
