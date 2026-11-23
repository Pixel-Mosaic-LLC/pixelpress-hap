#!/bin/bash
# Builds PixelPress HAP for Apple Silicon Macs and packages an installer (.pkg).
#
# Needs: Xcode (full, not just Command Line Tools - ibtool/Rez are required),
#        CMake 3.15+ (brew install cmake), and the Adobe SDKs unpacked into
#        external/foundation/external/adobe/AfterEffectsSDK and .../premiere
#
# Optional signing (removes Gatekeeper warnings; needs Apple Developer Program):
#   PPHAP_CODESIGN_IDENTITY="Developer ID Application: Name (TEAMID)"
#   PPHAP_INSTALLER_IDENTITY="Developer ID Installer: Name (TEAMID)"
#   PPHAP_NOTARY_PROFILE=<keychain profile from: xcrun notarytool store-credentials>
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)
# 1.2.3: the project may sit in a folder with spaces (e.g. ~/Projects/Pixel Press Plugin/Source Code/
# PixelPressHAP). FFmpeg's configure refuses such paths, so the build runs through a link without
# spaces (/tmp/pphap-src-<user> -> the project); everything it writes still lands in the project.
if [[ "$ROOT" == *" "* ]] && [ -z "${PPHAP_BUILD_LINK:-}" ]; then
  LINK="/tmp/pphap-src-$(id -un)"
  ln -sfn "$ROOT" "$LINK"
  echo "Project folder has spaces - building through $LINK"
  PPHAP_BUILD_LINK=1 bash "$LINK/scripts/build-mac.sh" "$@"
  exit $?
fi
ARCH="${ARCH:-arm64}"
BUILD="build-mac-$ARCH"
# a build folder set up from another path (the project was moved / copied from another machine):
# CMake refuses it, so start it fresh
for d in "$BUILD" "build-selftest-$ARCH"; do
  if [ -f "$d/CMakeCache.txt" ] && ! grep -qxF "CMAKE_HOME_DIRECTORY:INTERNAL=$ROOT$( [ "$d" = "$BUILD" ] || echo /tools)" "$d/CMakeCache.txt"; then
    echo "$d was set up for another folder - starting it fresh"; rm -rf "$d"
  fi
done
fail() { echo; echo "✗ $*"; exit 1; }
# CMake 4 (current Homebrew) refuses the old cmake_minimum_required() in bundled libraries such as snappy;
# this lets it configure them anyway (ignored by CMake 3).
export CMAKE_POLICY_VERSION_MINIMUM=3.5

command -v xcrun >/dev/null && xcrun --find ibtool >/dev/null 2>&1 || fail "Full Xcode is required (App Store), then: sudo xcode-select -s /Applications/Xcode.app"
command -v cmake >/dev/null || fail "CMake not found: brew install cmake"
ADOBE=external/foundation/external/adobe
[ -f "$ADOBE/AfterEffectsSDK/Examples/Headers/AE_GeneralPlug.h" ] || fail "After Effects SDK missing: unpack it so that $ADOBE/AfterEffectsSDK/Examples/Headers exists"
[ -f "$ADOBE/premiere/Headers/PrSDKExport.h" ] || fail "Premiere Pro SDK missing: unpack it so that $ADOBE/premiere/Headers exists"
grep -q "CodeMacARM64" "$ADOBE/AfterEffectsSDK/Examples/Resources/AE_General.r" 2>/dev/null \
  || fail "This After Effects SDK is too old for Apple Silicon (no CodeMacARM64). Download the current SDK from developer.adobe.com/console/servicesandapis"

bash scripts/fetch-deps.sh
bash scripts/build-ffmpeg-mac.sh "$ARCH"

# Intel ISPC compiler for the bc7e (Hap R) encoder
ISPC_VER=1.25.3
ISPC_DIR="$ROOT/.tools/ispc-$ISPC_VER"
if [ ! -x "$ISPC_DIR/bin/ispc" ]; then
  echo "Downloading ISPC $ISPC_VER ..."
  mkdir -p "$ROOT/.tools" && rm -rf "$ISPC_DIR"
  curl -fsSL "https://github.com/ispc/ispc/releases/download/v$ISPC_VER/ispc-v$ISPC_VER-macOS.universal.tar.gz" | tar -xz -C "$ROOT/.tools"
  mv "$ROOT/.tools/$(ls "$ROOT/.tools" | grep -m1 "^ispc-v$ISPC_VER")" "$ISPC_DIR"
fi
"$ISPC_DIR/bin/ispc" --version | head -1

# Pick the faster ISPC NEON width for bc7e on this Apple Silicon CPU (both give identical output;
# the stress test below still checks the final build). Override with BC7E_NEON_TARGET=neon-i32x4|neon-i32x8.
NEON_TARGET="${BC7E_NEON_TARGET:-}"
if [ "$ARCH" = "arm64" ] && [ -z "$NEON_TARGET" ]; then
  echo "Timing bc7e NEON targets on this Mac ..."
  NB="$ROOT/.tools/neon-bench"; rm -rf "$NB"; mkdir -p "$NB"
  BEST_MS=""; NEON_TARGET=neon-i32x4
  for t in neon-i32x4 neon-i32x8; do
    mkdir -p "$NB/$t"
    if "$ISPC_DIR/bin/ispc" external/bc7e/bc7e.ispc -O2 --arch=aarch64 --target=$t --target-os=macos --pic \
         -o "$NB/$t/bc7e.o" -h "$NB/$t/bc7e_ispc.h" >/dev/null 2>&1 &&
       xcrun clang++ -std=c++17 -O2 -arch arm64 -I "$NB/$t" tools/bc7e_neon_bench.cpp "$NB/$t/bc7e.o" -o "$NB/$t/bench" &&
       OUT=$("$NB/$t/bench" | tail -1); then
      MS=$(echo "$OUT" | sed -E 's/^ms=([0-9.]+).*/\1/')
      echo "  $t: $OUT  (1080p frame, all cores)"
      if [ -z "$BEST_MS" ] || awk "BEGIN{exit !($MS < $BEST_MS)}"; then BEST_MS=$MS; NEON_TARGET=$t; fi
    else
      echo "  $t: failed - skipped"
    fi
  done
fi
[ "$ARCH" = "arm64" ] && echo "bc7e NEON target: ${NEON_TARGET:-neon-i32x4}"
NEON_ARG=(); [ -n "$NEON_TARGET" ] && NEON_ARG=(-DBC7E_NEON_TARGET="$NEON_TARGET")

cmake -S "$ROOT" -B "$ROOT/$BUILD" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="$ARCH" -DISPC_EXECUTABLE="$ISPC_DIR/bin/ispc" ${NEON_ARG[@]+"${NEON_ARG[@]}"}
cmake --build "$ROOT/$BUILD" --config Release -j"$(sysctl -n hw.ncpu)"

# Encoder self-test on this CPU before anything is packaged (fails the build on any encoder problem)
echo "Running encoder stress test ..."
cmake -S "$ROOT/tools" -B "$ROOT/build-selftest-$ARCH" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="$ARCH" \
      -DISPC_EXECUTABLE="$ISPC_DIR/bin/ispc" ${NEON_ARG[@]+"${NEON_ARG[@]}"} >/dev/null
cmake --build "$ROOT/build-selftest-$ARCH" --config Release -j"$(sysctl -n hw.ncpu)" >/dev/null
VENV="$ROOT/.tools/venv"
[ -x "$VENV/bin/python" ] || python3 -m venv "$VENV"
"$VENV/bin/python" -m pip install --quiet --disable-pip-version-check numpy scikit-image \
  || "$VENV/bin/python" -m pip install --quiet --disable-pip-version-check numpy
"$VENV/bin/python" tools/stress.py "$ROOT/build-selftest-$ARCH/hapr_selftest" || fail "Encoder stress test failed - not packaging this build"

# Sign the plugin bundles (ad-hoc unless a Developer ID is supplied)
ID="${PPHAP_CODESIGN_IDENTITY:--}"
TS="--timestamp"; [ "$ID" = "-" ] && TS="--timestamp=none"   # notarization needs a secure timestamp
find "$BUILD" -maxdepth 9 \( -name "PPHAP*.bundle" -o -name "PPHAP*.plugin" \) -type d | while read -r b; do
  codesign --force --deep $TS --options runtime --sign "$ID" "$b"
  echo "✓ signed $(basename "$b") ($ID)"
done
PPHAP_BIN=$(find "$BUILD" -maxdepth 4 -type f -name pphap -perm -u+x | head -1)
[ -n "$PPHAP_BIN" ] || fail "pphap command-line tool was not built"
codesign --force $TS --options runtime --sign "$ID" "$PPHAP_BIN" && echo "✓ signed pphap"

cd "$BUILD"
cpack -G productbuild -C Release
PKG=$(ls -t PixelPressHAP-*.pkg | head -1)
if [ -n "${PPHAP_NOTARY_PROFILE:-}" ]; then
  xcrun notarytool submit "$PKG" --keychain-profile "$PPHAP_NOTARY_PROFILE" --wait
  xcrun stapler staple "$PKG"
fi
mkdir -p "$ROOT/dist" && cp "$PKG" "$ROOT/dist/" && cp "$ROOT/$PPHAP_BIN" "$ROOT/dist/pphap"
echo; echo "✓ Installer: $ROOT/dist/$PKG"
