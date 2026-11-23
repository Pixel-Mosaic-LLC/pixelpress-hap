#!/bin/bash
# Fetches the third-party sources that aren't stored in this repo.
set -euo pipefail
cd "$(dirname "$0")/.."
EXT=external/foundation/external
clone() { # url branch/tag dest
  if [ ! -e "$3/.fetched" ]; then
    rm -rf "$3"; echo "Fetching $3 ..."
    git clone -q --depth 1 --branch "$2" "$1" "$3"
    touch "$3/.fetched"
  fi
}
clone https://github.com/FFmpeg/FFmpeg.git            n4.4.8      $EXT/ffmpeg/FFmpeg
clone https://github.com/nlohmann/json.git            v3.11.3     $EXT/json
if [ "${1:-}" = "--with-tests" ]; then
  clone https://github.com/google/googletest.git      v1.14.0     $EXT/googletest
fi
echo "✓ dependencies ready"
