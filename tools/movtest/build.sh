#!/bin/bash
# Builds movtest: the plugin's full encode + .mov write path, outside Adobe (Linux/macOS).
# Prereqs: scripts/fetch-deps.sh, a built FFmpeg in external/foundation/external/ffmpeg/FFmpeg,
#          and the self-test libraries: cmake -S tools -B build-selftest && cmake --build build-selftest
set -euo pipefail
cd "$(dirname "$0")"
R=$(cd ../.. && pwd); F=$R/external/foundation/source
FF=${FFMPEG_DIR:-$R/external/foundation/external/ffmpeg/FFmpeg}
JS=${JSON_DIR:-$R/external/foundation/external/json}
B=${SELFTEST_BUILD:-$R/build-selftest}
OUT=${OUT:-$R/build-selftest/movtest}
c++ -std=c++17 -O2 -o "$OUT" movtest.cpp log_stub.cpp \
  $R/codec/codec.cpp $F/codec_registration/util.cpp \
  $F/session/movie_writer.cpp $F/session/ffmpeg_helpers.cpp $F/session/movie_reader.cpp \
  -I$R/codec -I$F/codec_registration -I$F/session -I$FF -I$R/external/hap \
  -I$R/external/squish/squish-source -I$R/external/ycocg -I$B/bc7e -I$JS/single_include \
  $B/libselftest_codec.a $B/hap/libHap.a $B/snappy/libsnappy.a $B/squish/libsquish.a $B/ycocg/libycocg.a \
  $FF/libavformat/libavformat.a $FF/libavcodec/libavcodec.a $FF/libavutil/libavutil.a -lpthread -lm
echo "built $OUT"
