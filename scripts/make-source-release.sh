#!/bin/bash
# Builds the PUBLIC source packages that go out next to every installer (LGPL v2.1 compliance):
#   dist/PixelPressHAP-<version>-source.zip         this project, WITHOUT the Adobe SDKs
#   dist/PixelPressHAP-<version>-ffmpeg-source.zip  the exact FFmpeg source the installers link, plus
#                                                   (1.1.7) the sources of the Windows ffmpeg.exe / ffprobe.exe
# Usage: scripts/make-source-release.sh            (needs git, zip, python3)
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)
VER=$(sed -n 's/^set(CPACK_PACKAGE_VERSION "\(.*\)")/\1/p' installer/CodecInstaller.cmake)
FFTAG=$(sed -n 's/^clone https:\/\/github.com\/FFmpeg\/FFmpeg.git *\([^ ]*\).*/\1/p' scripts/fetch-deps.sh)
[ -n "$VER" ] && [ -n "$FFTAG" ] || { echo "could not read version / FFmpeg tag"; exit 1; }
mkdir -p dist
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
NAME=PixelPressHAP-$VER-source

# 1. project source - everything except Adobe SDKs, build output and fetched third-party trees
python3 - "$ROOT" "$TMP/$NAME" <<'PY'
import shutil, sys, pathlib
src, dst = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
skip_dirs = {'.git', '.tools', 'dist', 'build', 'Release', 'Debug', 'Xcode', '.vscode', '__MACOSX', '.build-win-cross'}
skip_paths = {'external/foundation/external/adobe/AfterEffectsSDK', 'external/foundation/external/adobe/premiere',
              'external/adobe/premiere', 'external/foundation/external/ffmpeg/FFmpeg',
              'external/foundation/external/json', 'external/foundation/external/StackWalker',
              'external/foundation/external/googletest'}
def ignore(d, names):
    rel = pathlib.Path(d).relative_to(src)
    out = []
    for n in names:
        p = (rel / n).as_posix()
        if n in skip_dirs or n.startswith('build-') or n == '.DS_Store' or p in skip_paths: out.append(n)
        elif rel.as_posix() == 'adobe-sdk' and n != 'README.md': out.append(n)   # Adobe licence: never publish
    return out
shutil.copytree(src, dst, ignore=ignore)
PY
# belt and braces: refuse to publish anything that looks like an Adobe SDK
if find "$TMP/$NAME" -iname '*SDK*.zip' -o -name 'AE_GeneralPlug.h' -o -name 'PrSDKExport.h' | grep -q .; then
  echo "ERROR: Adobe SDK files found in the source package - not publishing"; exit 1; fi
(cd "$TMP" && rm -f "$ROOT/dist/$NAME.zip" && zip -qr -X "$ROOT/dist/$NAME.zip" "$NAME")

# 2. FFmpeg - the exact, unmodified upstream source
git -c advice.detachedHead=false clone -q --depth 1 --branch "$FFTAG" https://github.com/FFmpeg/FFmpeg.git "$TMP/FFmpeg-$FFTAG"
rm -rf "$TMP/FFmpeg-$FFTAG/.git"
cp scripts/build-ffmpeg-mac.sh scripts/build-ffmpeg-win.sh "$TMP/FFmpeg-$FFTAG/"
cat > "$TMP/FFmpeg-$FFTAG/PIXELPRESS-README.txt" <<TXT
Unmodified FFmpeg $FFTAG source, as linked into PixelPress HAP $VER.
Built with build-ffmpeg-mac.sh / build-ffmpeg-win.sh (LGPL configuration, no --enable-gpl).
To relink PixelPress HAP against your own build: put this tree at
external/foundation/external/ffmpeg/FFmpeg in PixelPressHAP-$VER-source and run its build scripts.
TXT
# 3. 1.1.7: the sources of the Windows installer's ffmpeg.exe / ffprobe.exe (scripts/build-ffmpeg-win-exe.sh)
WF=$(sed -n 's/^FFV=\([^;]*\);.*/\1/p' scripts/build-ffmpeg-win-exe.sh)
WS=$(sed -n 's/.*SNV=\([^;]*\);.*/\1/p' scripts/build-ffmpeg-win-exe.sh)
WZ=$(sed -n 's/.*ZV=\([0-9.]*\)$/\1/p' scripts/build-ffmpeg-win-exe.sh)
[ -n "$WF" ] && [ -n "$WS" ] && [ -n "$WZ" ] || { echo "could not read the Windows ffmpeg versions"; exit 1; }
WD="$TMP/windows-ffmpeg-exe"; mkdir -p "$WD"
getsrc() {   # repo tag file  - release archive, or a git checkout packed the same way
  curl -fsSL --retry 3 -o "$WD/$3" "https://github.com/$1/archive/refs/tags/$2.tar.gz" 2>/dev/null && return
  rm -rf "$TMP/g"; git -c advice.detachedHead=false clone -q --depth 1 --branch "$2" "https://github.com/$1.git" "$TMP/g/${3%.tar.gz}"
  rm -rf "$TMP/g/${3%.tar.gz}/.git"; tar -czf "$WD/$3" -C "$TMP/g" "${3%.tar.gz}"
}
getsrc FFmpeg/FFmpeg "n$WF" "FFmpeg-n$WF.tar.gz"
getsrc google/snappy "$WS" "snappy-$WS.tar.gz"
getsrc madler/zlib "v$WZ" "zlib-$WZ.tar.gz"
cp scripts/build-ffmpeg-win-exe.sh "$WD/"
cat > "$WD/README.txt" <<TXT
Sources of ffmpeg.exe / ffprobe.exe that the PixelPress HAP $VER Windows installer puts in
C:\Program Files\PixelPress: FFmpeg $WF (LGPL v2.1+, unmodified), snappy $WS (BSD-3), zlib $WZ.
Built with build-ffmpeg-win-exe.sh (MinGW-w64 cross-compile, LGPL configuration, no --enable-gpl);
the exact configure line and checksums are in ffmpeg-build-info.txt next to the installed programs.
TXT

(cd "$TMP" && rm -f "$ROOT/dist/PixelPressHAP-$VER-ffmpeg-source.zip" && zip -qr -X "$ROOT/dist/PixelPressHAP-$VER-ffmpeg-source.zip" "FFmpeg-$FFTAG" windows-ffmpeg-exe)

ls -lh dist/PixelPressHAP-$VER-*source.zip
echo "✓ publish both zips next to the installers"
