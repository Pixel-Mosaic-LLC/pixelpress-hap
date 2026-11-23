#!/bin/bash
# Called by build-win.ps1 inside MSYS2 with the MSVC x64 environment inherited.
# Minimal static FFmpeg 4.4 (QuickTime muxer/demuxer only, LGPL) built with MSVC.
set -euo pipefail
cd "$(dirname "$0")/../external/foundation/external/ffmpeg/FFmpeg"
command -v cl.exe >/dev/null || { echo "MSVC cl.exe not on PATH (vcvars64 not applied)"; exit 1; }
./configure --toolchain=msvc --arch=x86_64 --disable-asm --disable-network --disable-everything \
  --enable-muxer=mov --enable-demuxer=mov --disable-zlib --disable-iconv --disable-programs \
  --disable-doc --disable-autodetect --extra-cflags=-MD >/dev/null
make -j"$(nproc)" >/dev/null
echo "FFmpeg built"
