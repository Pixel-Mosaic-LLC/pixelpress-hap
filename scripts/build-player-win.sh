#!/bin/bash
# PixelPress Player (Windows, tools/player) - cross-built like scripts/build-win-cross.sh (run that
# first: it makes the Hap / snappy / GDeflate libraries this links).
#   bash scripts/build-player-win.sh [OUTDIR]          (default OUTDIR: dist/win-cross)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$(mkdir -p "${1:-$ROOT/dist/win-cross}" && cd "${1:-$ROOT/dist/win-cross}" && pwd)"
WORK="${PP_WINX_WORK:-$ROOT/.build-win-cross}"
VER=0.2.0
HOST=x86_64-w64-mingw32
CXX=$HOST-g++-posix; command -v $CXX >/dev/null || CXX=$HOST-g++
CC=$HOST-gcc-posix; command -v $CC >/dev/null || CC=$HOST-gcc
JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
B="$WORK/build"
[ -f "$B/external/hap/libHap.a" ] || { echo "run scripts/build-win-cross.sh first"; exit 1; }
# the player's FFmpeg: mov demuxer + audio decoders (PCM, AAC, MP3) + resampler, LGPL
FF="$WORK/ffmpeg-player"
if [ ! -f "$FF/libswresample/libswresample.a" ]; then
  rm -rf "$FF"; cp -R "$ROOT/external/foundation/external/ffmpeg/FFmpeg" "$FF"
  (cd "$FF" && (make distclean >/dev/null 2>&1 || true) &&
   ./configure --target-os=mingw32 --arch=x86_64 --cross-prefix=$HOST- --cc=$CC --disable-asm --disable-network \
     --disable-everything --enable-protocol=file --enable-demuxer=mov \
     --enable-decoder=aac,aac_latm,pcm_s16le,pcm_s16be,pcm_s24le,pcm_s24be,pcm_s32le,pcm_s32be,pcm_f32le,pcm_f32be,mp3float \
     --enable-parser=aac --enable-swresample --disable-zlib --disable-iconv --disable-programs --disable-doc \
     --disable-autodetect --enable-static --disable-shared --disable-avdevice --disable-swscale --disable-avfilter \
     --disable-postproc >/dev/null && make -j"$JOBS" >/dev/null)
fi
$CXX -O2 -std=c++17 -municode -DPPPLAYER_VERSION="\"$VER\"" -I"$FF" -I"$ROOT/external/hap" -I"$ROOT/codec" \
  -I"$ROOT/external/gdeflate/GDeflate" "$ROOT/tools/player/ppplayer.cpp" "$ROOT/codec/gdeflate_hap.cpp" \
  "$B/external/hap/libHap.a" "$B/external/gdeflate/libGDeflate.a" "$B/external/snappy/libsnappy.a" \
  "$FF/libavformat/libavformat.a" "$FF/libavcodec/libavcodec.a" "$FF/libswresample/libswresample.a" "$FF/libavutil/libavutil.a" \
  -static -ld3d11 -ldxgi -lole32 -lwinmm -lbcrypt -luuid -lshell32 -lgdi32 -luser32 -lws2_32 -o "$OUT/ppplayer.exe"
$HOST-strip "$OUT/ppplayer.exe"
echo "✓ $OUT/ppplayer.exe"
