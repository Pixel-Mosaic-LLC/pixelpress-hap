# PixelPress HAP — render-time Hap / Hap R export for Adobe CC

Export **Hap, Hap Alpha, Hap Q, Hap Q Alpha, Hap Alpha-Only and Hap R** QuickTime
movies straight from the After Effects render queue (and Premiere Pro / Media Encoder),
natively on **Windows x64** and **Apple Silicon Macs** (M-series, macOS 11+).

Built on the BSD-licensed [disguise HAP encoder for Adobe CC](https://github.com/disguise-one/hap-encoder-adobe-cc)
(see `README.upstream.md`). What this fork adds:

| | |
|---|---|
| **Hap R** (`Hap7`, BC7/BPTC, RGBA) | new sub-codec using Binomial's SIMD **bc7e** encoder (ISPC: ARM NEON on Apple Silicon, SSE4/AVX2 on x64); Snappy, 8 chunks per frame (same layout as Jokyo / ffmpeg output); opaque pixels stay exactly 255 alpha |
| **Multi-core per frame** | every format splits each frame into bands encoded on all cores (shared pool, output identical to single-threaded) |
| **Apple Silicon** | arm64 build, `CodeMacARM64` PiPL entry for After Effects, no x86 SIMD flags, no Boost (macOS 11 std::filesystem), FFmpeg 4.4 |
| **Coexists with the disguise plugin** | own plugin names/IDs (`PPHAP…`, `com.pixelmosaic.PixelPressHAP`); its installers no longer delete the disguise plugin |
| **Quality setting** | Fast / Normal / High (slow) for every format |
| **Build tooling** | one-command Mac & Windows builds (local, or GitHub Actions) |

## What's new in 1.1 — colour accuracy
Every encoded frame is now decoded again in testing, exactly as a media server's GPU decodes it,
and compared with the source. Measured on a 1080p test set (ProRes 422 HQ and untagged H.264
sources), end to end against the RGB master, default Fast setting:

| Format | 1.0 mean ΔE2000 | 1.1 mean ΔE2000 | luma PSNR 1.0 → 1.1 |
|---|---|---|---|
| Hap | 1.15 | 0.73 | 37.8 → 40.6 dB |
| Hap Alpha | 1.15 | 0.75 | 37.8 → 40.5 dB |
| Hap Q | 0.98 | 0.65 | 40.9 → 46.7 dB |

1.1.3 adds a Hap Q rounding fix on top of this (mean ΔE a further ~20% lower, luma +1.3 dB, no colour cast).

(ΔE2000 1.0 ≈ the smallest difference a viewer can notice. Hap R's encoder is unchanged; it gets
the colour-conversion fix below.)

* **YUV → RGB conversion (pphap)** — ffmpeg's default conversion takes a fast path that drops the
  extra precision of 10-bit sources, and can use the wrong matrix for untagged files. pphap now
  reads the file's matrix and range tags (untagged: BT.709 for HD and larger, BT.601 for SD), uses
  full-precision rounding and chroma, and prints what it used. Override with
  `-colorspace bt709|bt601|bt2020` and `-color_range tv|pc`. HDR (PQ/HLG) sources get a warning.
* **Hap / Hap Alpha** — rgbcx BC1/BC3 encoder replaces squish. Fast is +2.2 dB at the same speed;
  Normal is better than the old High and faster.
* **Hap Q** — Y is encoded with a high-quality BC4 search; Co/Cg with a least-squares endpoint fit
  scored by the exact RGB error of the Hap Q shader. About 2–3x the encode time of 1.0's real-time encoder at Fast (1.1.2); playback cost is unchanged.
* **Alpha planes** (Hap Alpha, Hap Q Alpha, Alpha-Only) — high-quality BC4, about +1.4 dB.
* **Quality setting for every format** (Hap Q and alpha-only included).
* **`pphap -psnr`** — reports PSNR and ΔE2000 of what was actually encoded.

### Falling back to 1.0
The 1.0.0 encoders are still inside 1.1 and produce byte-identical 1.0 output:
* pphap: `-legacy` (both), or `-engine legacy` / `-colorconv legacy` separately.
* Adobe plugin: set the environment variable `PPHAP_ENGINE=legacy` before launching After Effects /
  Premiere / Media Encoder (Mac: `launchctl setenv PPHAP_ENGINE legacy`, then restart the app;
  Windows: System Properties › Environment Variables).

## Hap R quality settings

Measured against the previous bc7enc-based encoder (RGB PSNR, 1080p): every setting is at least as
good as the old setting of the same name, and much faster.

| Setting | bc7e preset | vs old same setting | single-core speed vs old Normal |
|---|---|---|---|
| Fast | veryfast | +0.2 to +0.7 dB | ~7x faster |
| Normal (default) | basic | +0.1 to +0.7 dB | ~4x faster |
| High | slow | +0.3 to +5.5 dB | ~2x faster |

...then multiplied by the number of CPU cores, since each frame is now split across all of them.

## Using it
After Effects: **Render Queue › Output Module › Format: "PixelPress HAP Movie"** ›
Format Options › choose **Hap R** (default) or another variant, quality and chunk count
(Auto = 8 chunks for Hap R, 1 for the others).

## Command line: `pphap`
The installers also include **`pphap`**, a command-line encoder built from the same codec as the
plugin (same output, byte for byte, on the same platform; Hap R bytes differ invisibly between Apple
Silicon and Intel builds, at identical quality). Mac: installed to `/usr/local/bin/pphap`. Windows: `pphap.exe`
is in the download next to the installer, and the installer puts it in a fixed place (1.1.7, below).

**Windows install location (1.1.7).** The plugin itself has to live in Adobe's folder
(`C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\PPHAP`), a path Adobe controls. So that
other programs (e.g. Smart Preso) can rely on a path, the installer also puts `pphap.exe`, an LGPL
`ffmpeg.exe` / `ffprobe.exe` (built by `scripts/build-ffmpeg-win-exe.sh`) and the licences in
**`C:\Program Files\PixelPress`**, and records it in the registry (64-bit view):

| `HKLM\SOFTWARE\Pixel Mosaic\PixelPress HAP` | |
| --- | --- |
| `PphapPath` | `C:\Program Files\PixelPress\pphap.exe` |
| `InstallDir` | `C:\Program Files\PixelPress` |
| `Version` | e.g. `1.1.8` |

The key has no version in its name, so it stays the same across releases; upgrading replaces the
folder and values, uninstalling removes both. (`PluginDir` is `...\MediaCore\PPHAP` from 1.1.8; 1.1.7 wrote
`...\MediaCore\PixelPressHAP`, which doesn't exist.) From 1.1.8 the Visual C++ runtime DLLs pphap.exe needs
are installed next to it, so it runs on a PC that has no Adobe apps or VC++ redistributable.

ffmpeg-style options; decoding of `-i FILE` is done by `ffmpeg`/`ffprobe` (next to pphap, on PATH,
Homebrew, or `$PPHAP_FFMPEG` / `$PPHAP_FFPROBE`):

```bash
pphap -i show.mov -c:v hap_r show_HapR.mov                       # Hap R (default quality normal)
pphap -i show.mov -c:v hap -format hap_q -y show_HapQ.mov        # ffmpeg-style Hap Q
pphap -i show.mov -c:v hap_alpha -quality high -chunks 4 out.mov
pphap -i show.mov -vf scale=1920:1080 -r 30 -c:v hap_r out.mov   # filter/rate while decoding
pphap -i show.mov -ss 117.284 -to 121.822 -c:v hap_r out.mov     # 1.1.5: IN / OUT points (frame-accurate)
pphap -i show.mov -ss 00:01:57.284 -t 4.5 -c:v hap_r out.mov     #        or IN point + length

# 1.1.5: the form Smart Preso's CLI uses - options inside -vf are understood too
pphap -i show.mov -vf "-s 1920:1080 -ss 117.284 -to 121.822,format=rgba" -pix_fmt rgba \
  -chunks 8 -c:v hap -format hap_r -y out.mov -progress pipe:1

# raw frames from any source on stdin
ffmpeg -i in.mov -f rawvideo -pix_fmt rgba - | \
  pphap -f rawvideo -pix_fmt rgba -s 1920x1080 -r 30000/1001 -i - -c:v hap_r out.mov

# 1.1.7: Hap R input (ffmpeg can't decode Hap R; pphap does it itself)
pphap -i show_HapR.mov -c:v hap_q -y show_HapQ.mov                # re-encode to another Hap
pphap -i show_HapR.mov -vf scale=1280:720 -ss 10 -t 5 -c:v hap_r -y clip.mov
pphap -i show_HapR.mov -f rawvideo -pix_fmt rgba - | \
  ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -r 30000/1001 -i - -c:v prores_ks show.mov
pphap -ss 12.5 -i show_HapR.mov -frames:v 1 -f rawvideo -pix_fmt rgba frame.rgba   # one frame
```

**Decoded output (1.1.7).** With `-f rawvideo` after `-i`, or `-` as the output, pphap writes the
decoded frames instead of a Hap movie: packed pixels, no header, `-pix_fmt rgba` (default), `bgra`,
`argb` or `rgb24`, one frame after another (`width × height × 4` bytes for the 4-byte formats).
Works for any input; Hap R files are read and decoded by pphap itself (the plugin's QuickTime reader +
BC7 decoding), every other format by ffmpeg. `-ss` / `-to` / `-t` / `-vf` / `-frames:v` apply as for
encoding. With frames on stdout, send `-progress` to `pipe:2` or a file. If the reading program closes
the pipe early (e.g. after taking one frame) pphap stops quietly with exit code 0.
A damaged Hap R frame doesn't stop the job: pphap warns and repeats the previous frame (as ffmpeg does).

Codecs: `hap`, `hap_alpha`, `hap_q`, `hap_q_alpha`, `hap_alpha_only`, `hap_r`.
Audio (1.1.7): like ffmpeg, the source's first audio track is included unless `-an`, as uncompressed
PCM (`-c:a pcm_s16le`, the default, or `pcm_s32le`), cut to start on the first frame and to last as
long as the video, so trims stay in sync.
In / out points (1.1.5): `-ss TIME`, `-to TIME`, `-t DURATION` (seconds or `[HH:]MM:SS[.ms]`); the
first frame is the first one at or after `-ss`, the last the last one before `-to`, and `total_frames` /
`percent` count only that range.
Other options: `-quality fast|normal|high`, `-gpu off|auto|on` (experimental, below), `-chunks N`, `-alpha 0|1`, `-threads N`, `-frames:v N`,
`-y`/`-n`, `-nostats`, `-loglevel quiet`, `-h`. Colour (1.1): `-colorspace`, `-color_range`,
`-colorconv accurate|legacy`, `-engine accurate|legacy`, `-legacy`, `-psnr`.

**GPU encoding (experimental, off by default since 1.2.1).** `-gpu auto|on` runs Hap R at
`-quality fast` through a Vulkan compute shader (`codec/shaders/bc7_encode.comp`). Measured on an
RTX 3080 against an i7-13700K it was about 10x slower than the CPU encoder and slightly less
accurate on real footage (52.1 vs 52.5 dB), so 1.2.1 leaves it off. `-gpu auto` times both
before the first frame and keeps the faster; `-gpu on` forces it; `PPHAP_GPU=off|auto|on` sets the
same for programs that start pphap. A frame the GPU fails on is encoded on the CPU.

**Progress for other programs** — `-progress pipe:1` (stdout), `pipe:2` or a file path writes the
same key=value blocks as `ffmpeg -progress`, plus `total_frames` and `percent` when known:

```
frame=120
fps=31.52
total_frames=360
percent=33.3
out_time_us=4004000
out_time_ms=4004000
out_time=00:00:04.004000
total_size=31415926
bitrate=62768.1kbits/s
speed=1.05x
progress=continue        # ...last block says progress=end
```
`-stats_period SEC` sets the interval (default 0.5 s). The human status line goes to stderr
(`-nostats` to hide). Exit code 0 = success, 1 = error, 255 = interrupted (Ctrl-C, SIGTERM, or on
Windows Ctrl+Break / `GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT)`; the file is finalised and playable
up to the last frame written).

Frame rate: taken from the source (`r_frame_rate`); since 1.1.5 a value above 240 fps (a container
clock, e.g. 1000/1) is replaced by the measured average rate. `-r` always wins.


You need the Adobe **After Effects SDK** and **Premiere Pro SDK** (free, Adobe account):
<https://developer.adobe.com/console/servicesandapis>. They are licensed to you by Adobe,
so they must never be committed: `adobe-sdk/` is git-ignored except for its README.

Every installer is named with its version: `PixelPressHAP-<version>-macOS-AppleSilicon.pkg` and
`PixelPressHAP-<version>-Windows-x64.exe`. The SDK zips in `adobe-sdk/` are picked up by
`scripts/get_adobe_sdk.py` with no arguments.

### Option A – on Windows (no GitHub needed)
One-time setup, in PowerShell as Administrator:
```powershell
winget install --id Microsoft.VisualStudio.2022.BuildTools --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
winget install --id Git.Git; winget install --id Python.Python.3.12; winget install --id NSIS.NSIS
winget install --id MSYS2.MSYS2; winget install --id Kitware.CMake
```
The project can be anywhere, including folders with spaces (1.2.3: the script then builds through a
temporary drive letter). It needs
`ffmpeg-win\ffmpeg.exe` in `Pixel Press Plugin\3rd Party\` or in `dist\`, `ffprobe.exe` and `build-info.txt` (the LGPL ffmpeg the installer puts in
`C:\Program Files\PixelPress`; built on Linux by `scripts/build-ffmpeg-win-exe.sh`, and included in
the project zip). Then, in a normal PowerShell:
```powershell
cd "<path to>\PixelPressHAP"
py scripts\get_adobe_sdk.py
powershell -ExecutionPolicy Bypass -File scripts\build-win.ps1   # -> dist\PixelPressHAP-<version>-Windows-x64.exe
```
The first run builds FFmpeg (20-30 min); later runs take a few minutes. If CMake was configured with
bad settings, delete `build-win` and run it again.

**Just the Windows pphap.exe, without a Windows PC** (1.2.1): on Linux, or a Mac with
`brew install mingw-w64 cmake` and Wine (`brew install --cask wine-stable`):
```bash
bash scripts/build-win-cross.sh        # -> dist/win-cross/pphap.exe (static, no VC++ DLLs)
```
That is the exe the PixelPress app bundles and Smart Preso runs. Output is byte-identical to the
MSVC build. The plugin installer itself still needs Option A.

### Option B – on your Mac (Apple Silicon)
Full Xcode (App Store, or an older version from developer.apple.com/download/all if your macOS is
too old for the current one), then `sudo xcode-select -s /Applications/Xcode.app`, and:
```bash
brew install cmake nasm pkg-config
python3 scripts/get_adobe_sdk.py
bash scripts/build-mac.sh              # -> dist/PixelPressHAP-<version>-macOS-AppleSilicon.pkg
```
The script runs an encoder stress test before packaging and stops if it fails.

### Option C – GitHub Actions
Push to a **private** GitHub repository with a versioned project zip (`PixelPressHAP-<version>.zip`)
at the top level, or the project tree itself; Actions › *Build installers* › *Run workflow*. The
artifacts are named `PixelPressHAP-<version>-macOS-AppleSilicon` / `-Windows-x64`. This uses the
account's Actions minutes (macOS minutes count 10x), so Options A and B are the free route.

### Signing
Unsigned installers work but show a security prompt (Mac: System Settings › Privacy &
Security › Open Anyway). With an Apple Developer ID, set the `PPHAP_*` variables described
at the top of `scripts/build-mac.sh` (or the `MAC_*` secrets in the workflow) to sign and notarize.

## Encoder self-test (no Adobe SDK needed)
```bash
cmake -S tools -B build-selftest -DCMAKE_BUILD_TYPE=Release -DISPC_EXECUTABLE=/path/to/ispc && cmake --build build-selftest
build-selftest/hapr_selftest frame.rgba 1920 1080 1 8 out.hapframe
```
Encodes a raw RGBA frame through the plugin's Hap R path, round-trips it through the Hap
reference decoder and reports size and PSNR.

## Licences
BSD (disguise plugin, Codec Foundation, Hap), Apache 2.0 (bc7e, external/bc7e), MIT/public domain
(bc7enc decoder, self-test only), BSD (Snappy,
squish: MIT), LGPL 2.1 (FFmpeg — built with only the QuickTime muxer, statically linked;
the FFmpeg source used is fetched unmodified from the release/4.4 branch). See `license.txt`.

## Licence and public release

PixelPress HAP is free. The plugin is BSD-licensed (it builds on disguise's BSD plugin); bundled
libraries are BSD, MIT, Apache 2.0 and LGPL v2.1 - see `license.txt`, which the installers show
and install. Two LGPL libraries (FFmpeg and the YCoCg-DXT code) are linked statically, so every
public release must ship its source next to the installers:

    scripts/make-source-release.sh   # -> dist/PixelPressHAP-x.y.z-source.zip + -ffmpeg-source.zip

The Adobe SDK zips in `adobe-sdk/` are licensed to you by Adobe: never commit or publish them
(the folder is git-ignored). The source zips above leave them out (and the script refuses to build
one that contains them).

