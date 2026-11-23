#!/bin/bash
# Minimal static FFmpeg 4.4 (QuickTime muxer/demuxer only, LGPL) for Apple Silicon.
set -euo pipefail
cd "$(dirname "$0")/../external/foundation/external/ffmpeg/FFmpeg"
ARCH="${1:-arm64}"
MIN=11.0
if [ -f libavformat/libavformat.a ] && lipo -archs libavformat/libavformat.a 2>/dev/null | grep -qw "$ARCH"; then
  echo "✓ FFmpeg already built for $ARCH"; exit 0
fi
make distclean >/dev/null 2>&1 || true
./configure --arch="$ARCH" --cc="clang -arch $ARCH" --enable-cross-compile --target-os=darwin \
  --disable-asm --disable-network --disable-everything --enable-muxer=mov --enable-demuxer=mov \
  --disable-zlib --disable-iconv --disable-programs --disable-doc --disable-autodetect \
  --extra-cflags="-mmacosx-version-min=$MIN" --extra-ldflags="-mmacosx-version-min=$MIN" >/dev/null
make -j"$(sysctl -n hw.ncpu)" >/dev/null
echo "✓ FFmpeg built: $(lipo -archs libavformat/libavformat.a)"
