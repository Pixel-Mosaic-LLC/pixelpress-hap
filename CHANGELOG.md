# PixelPress HAP changelog

## 1.2.3 — 2026-10-30
- `pphap -h` crashed (1.2.2 only): the new help text's "%" signs were read as format codes.
  Nothing else changed; encoding and decoding are as in 1.2.2.

## 1.2.2 — 2026-10-30 (in PixelPress app 1.5.18 only)
- **GDeflate compression option** (`pphap -compressor gdeflate`, `-gdeflate_level 1-12`, default 6).
  GDeflate is Microsoft/NVIDIA's DEFLATE layout that DirectStorage unpacks on the GPU
  (external/gdeflate: the DirectStorage reference code, Apache-2.0, on NVIDIA's libdeflate, MIT).
  Same Hap frame layout and the same BC7/DXT textures (decoded pictures are bit-identical to
  Snappy files); only the chunk compression differs, marked 0xD in the chunk compressor table.
  4K Hap R test clip: 401 MB instead of 477 MB (-16%), encoding ~5% slower. **Not standard Hap**:
  disguise, Resolume, VLC etc. can't play these files; pphap can read them, and the PixelPress
  player will. Snappy stays the default.
- Hap R files with AAC (or other compressed) audio can be read again: pphap stopped with "no frames
  were read" on them (e.g. Hap R from ffmpeg-based encoders). The audio track is now ignored by the
  Hap R reader; pphap still carries the audio over through ffmpeg as before.

## 1.2.1 — 2026-10-29
Efficiency. Output is byte-identical to 1.2.0 on the same CPU (except AMD, below).
- GPU encoder off by default: measured on an RTX 3080 / i7-13700K it was ~10x slower than the CPU
  and slightly less accurate on real footage (52.1 vs 52.5 dB), and `auto` had to start Vulkan for
  every job to find that out. `-gpu auto` / `-gpu on` still turn it on.
- Read-ahead capped by memory: pphap kept 2 frames per CPU thread in flight. On a 48-thread
  Threadripper that is ~96 frames - about 12 GB at 8K, and the first finished frame only after
  ~100 frames had been decoded (minutes at 8K, shown as a long stall at frame 0). Now at most
  ~3 GB of frames: 4K ~40, 8K ~10, 1080p and smaller unchanged. Each frame is still split across
  all cores. A frame's input copy is also freed as soon as the encoder has its own.
- Frames are read on their own thread (a few frames ahead), so ffmpeg keeps decoding while pphap
  hands frames to the encoder and writes the results.
- AMD CPUs use the AVX2 build of the BC7 encoder. On a Threadripper PRO 7965WX (Zen 4) the AVX-512
  build that ISPC picks by itself was ~10% slower (4K: 11.1 vs 12.4 frames/s); Intel keeps AVX-512.
  `PPHAP_SIMD=avx2|avx512|auto` overrides; pphap's info line shows which one runs ("bc7e avx2").
  Same quality; Hap R bytes differ between the builds, as before.
- Faster pixel copies: the 8-bit ARGB -> RGBA copy before encoding (~10% of the CPU time of a 4K
  Hap R encode) and the raw `-pix_fmt rgba|bgra` input conversion work on whole pixels.
- Docs: the 1.2.0 GPU quality claim is corrected (see above).
- Build: `scripts/build-win-cross.sh` builds the Windows pphap.exe on Linux/Mac (MinGW-w64, with
  Intel's Windows ISPC run under Wine) - no Windows PC or Visual Studio needed. Its Hap / Hap Q /
  Hap R output is byte-identical to the MSVC build, and it is one static exe (no VC++ DLLs).
  The Adobe plugin installers are still built on Windows with `scripts/build-win.ps1`.

## 1.2.0 — 2026-10-28
Faster encoding (pphap, and so the PixelPress app and Smart Preso). Hap R output changes: see below.
- **GPU Hap R encoder.** pphap can encode Hap R on the graphics card (Vulkan; NVIDIA, AMD and
  Intel drivers on Windows include it, nothing to install). New BC7 compute shader written for
  PixelPress (`codec/shaders/bc7_encode.comp`): every block tries modes 6, 5 (4 rotations), 1 and 3
  on the best-estimated partitions, and 0 (opaque) or 7 (alpha), then refines the two best. It is
  **more accurate than the CPU's Fast setting** on every test picture (RGB PSNR, 2K):
  test chart +0.4 dB, its photo/text quarter +0.5 dB, camera video +0.6 dB, gradients + alpha
  +4 dB (alpha equal or better); opaque pixels stay exactly 255 alpha. Two independent BC7 decoders
  (bc7decomp, bcdec) agree on every block.
  `-gpu auto` (default) times the GPU against the CPU on a built-in test picture before the first
  frame and uses whichever is faster on that machine (the choice is printed); `-gpu on` always
  uses the GPU, `-gpu off` never; also `PPHAP_GPU=auto|on|off`. Used for Hap R at -quality fast
  (the default) only: Normal / High stay on the CPU (the GPU is not more accurate than those).
  A frame the GPU fails on is encoded on the CPU; no GPU or no Vulkan driver = CPU, as before.
  The Adobe plugins are unchanged (CPU).
- **AVX-512 CPU encoder.** The BC7 encoder is also built for AVX-512 (Intel Skylake-X and later,
  AMD Zen 4 / Threadripper 7000 and later) and picked at run time: 25-33% faster on Intel in our
  tests (Zen 4 gains less). Same quality (PSNR identical to 0.001 dB); a few blocks differ in the
  last bit from AVX2, so files from AVX-512 PCs are not byte-identical to 1.1.9.
- **Source conversion on all cores.** The colour conversion of video sources (YUV to RGB in ffmpeg)
  ran on one thread and capped pphap at about 60 frames/s at 2K regardless of the CPU. It is now
  split across all cores (ffmpeg 5.0 or later; bit-identical output).
- Linux test builds of pphap link again (settings path for Linux).

## 1.1.9 — 2026-10-19
Windows fixes. No change to encoder or decoder output (byte-identical to 1.1.8).
- pphap.exe: file names with characters outside the PC's ANSI code page (e.g. Japanese, Chinese, or
  "—") failed ("input not found"), and Hap R input failed for any non-ASCII path (é, ü ...). pphap
  now reads its arguments as Unicode and uses the Unicode Windows APIs for files and for starting
  ffmpeg / ffprobe, so any path works.
- After Effects plugin (Windows): rendering to a path with non-ASCII characters could fail to create
  the file (the UTF-8 path was opened with the ANSI API). Fixed in the shared file writer.
- pphap prints its messages immediately when they go to a pipe (Windows held them until pphap
  exited, so a program reading pphap's info line, such as the PixelPress app, had to wait).
- Settings file is read through the Unicode API (user folders with non-ASCII names).

## 1.1.8 — 2026-10-16
Fixes. No encoder or decoder output changes: files are byte-identical to 1.1.7.
- Windows: pphap.exe now runs on a PC without the Visual C++ runtime. It needs vcruntime140 /
  msvcp140, which Windows only has if something else installed them (Adobe apps do, so it worked on
  edit machines); the installer now puts those DLLs next to pphap.exe in C:\Program Files\PixelPress.
- Windows: the registry value PluginDir now names the real plugin folder (...\MediaCore\PPHAP).
- Windows: the damaged-frame warning gives the reason instead of "Unknown exception".
- Installer and download names carry the version: PixelPressHAP-1.1.8-macOS-AppleSilicon.pkg,
  PixelPressHAP-1.1.8-Windows-x64.exe, and the same in the GitHub artifact names.
- Local builds (no GitHub): build-win.ps1 loads the Visual Studio environment itself (works from a
  plain PowerShell, finds CMake / CPack), stops early if dist\ffmpeg-win is missing (an installer
  without ffmpeg.exe could otherwise be built), and both build scripts work with CMake 4. README has
  the one-time setup for each platform. 1.1.8 was built this way on Windows and checked: decoder
  checks, install over 1.1.7, registry values, uninstall.

## 1.1.7 — 2026-10-15
Decoding, plus robustness fixes found by testing it. No encoder changes: output is byte-identical
to 1.1.5. (1.1.6 was an internal build only; it lacks the fixes below and must not be distributed.)
- Hap decoder (Vidvox hap.c) hardened: a damaged frame could make it read outside the frame
  (chunk offsets / sizes were trusted; a section-size check could wrap at 32 bits; a signed shift
  overflowed). Every chunk is now checked against the frame. Fuzzed with 2 million damaged frames
  under AddressSanitizer / UndefinedBehaviorSanitizer: no crash, no memory error.
- A damaged Hap R frame no longer stops the job: like ffmpeg, pphap warns and repeats the previous
  frame, so timing and frame count stay right.
- Audio: pphap now writes the source's audio, as ffmpeg does by default (earlier versions were video
  only). First audio track, uncompressed PCM (pcm_s16le, or `-c:a pcm_s32le`), cut to start on the
  first output frame and to last exactly as long as the video (in / out points stay in sync to the
  sample). `-an` leaves it out.
- Windows installer: pphap.exe, plus LGPL ffmpeg.exe / ffprobe.exe (FFmpeg 7.1.1 with snappy + zlib,
  scripts/build-ffmpeg-win-exe.sh), are also installed in C:\Program Files\PixelPress, a fixed place
  other programs find through the registry: HKLM\SOFTWARE\Pixel Mosaic\PixelPress HAP (64-bit view)
  values PphapPath, InstallDir, Version, PluginDir. Nothing else needs installing for pphap.exe.
  Uninstalling removes both. The plugin itself stays in Adobe's MediaCore folder.
- pphap reads Hap R files. ffmpeg has no Hap R decoder, so a Hap R `-i` is now opened with the
  plugin's own QuickTime reader and decoded by pphap (Hap container / snappy -> BC7 -> RGBA),
  multithreaded (~120 fps at 1080p on 2 cores). Hap / Hap Alpha / Hap Q inputs are still decoded by
  ffmpeg. Hap R can therefore be re-encoded: to another Hap variant, trimmed (`-ss` / `-to` / `-t`,
  frame-accurate) or resized (`-vf`, filtered by ffmpeg after pphap decodes).
- pphap can write decoded frames: `-f rawvideo [-pix_fmt rgba|bgra|argb|rgb24]` after `-i`, or `-`
  as the output (stdout). For converting Hap R to ProRes / H.264 / PNG (pipe into ffmpeg) and for
  programs that want frames (e.g. one frame: `pphap -ss T -i in.mov -frames:v 1 -f rawvideo -`).
  `-progress` works as for encoding (to `pipe:2` or a file when frames go to stdout). A reader that
  closes the pipe early ends the job quietly (exit code 0).
- Decoded Hap R matches the encoder's own reference decode exactly (same PSNR per channel).
- In / out points: Hap R sources cut on exactly the frames ffmpeg picks for any other source
  (ffmpeg's rule, measured: a frame is kept if its time is >= IN and its time after IN, rounded to
  the nearest frame, is < the length), so a Hap R file and its ProRes master trim identically.
  `total_frames` in `-progress` is now the exact count for trimmed ffmpeg-decoded sources too.
- pphap 1.1.5 command lines behave as before, including raw piped input with its options after `-i`.
- Note (measured on the 1.1.7 builds): Hap R output differs slightly between Apple Silicon (Mac)
  and Intel / AMD (Windows) builds, because bc7e is compiled separately for each processor
  (NEON vs SSE / AVX). Quality is identical (same PSNR to 3 decimals, opaque alpha 255 on both;
  Mac vs Windows differ at ~85 dB). Within a platform the plugin and pphap are byte-identical.
  Hap / Hap Alpha / Hap Q encoding and all decoding are bit-identical on every platform.
- Known issues, fixed in 1.1.8: the Windows registry value PluginDir names
  ...\MediaCore\PixelPressHAP (the real folder is ...\MediaCore\PPHAP); on Windows the damaged-frame
  warning says "Unknown exception" instead of the reason; pphap.exe needs the Visual C++ runtime,
  which a PC without Adobe apps may not have.

## 1.1.5 — 2026-10-15
For Smart Preso integration, plus two fixes.
- pphap: IN / OUT points: `-ss`, `-to`, `-t` (seconds or `[HH:]MM:SS[.ms]`). Frame-accurate: the
  first frame is the first at or after `-ss`, the last the last before `-to`; `total_frames` /
  `percent` in `-progress` cover just that range.
- pphap: understands options inside the `-vf` string as Smart Preso's CLI sends them, e.g.
  `-vf "-s 1920:1080 -ss 117.284 -to 121.822,format=rgba"` (`-s` becomes a scale filter, RGB
  `format=` filters are dropped because pphap does its own accurate colour conversion).
- Hap R fix (Normal / High): opaque pixels could come out with alpha below 255 (seen: 239) in a
  few blocks. bc7e's mode 4/5 "rotation" moved alpha into the colour endpoints for opaque blocks;
  a rotated result is now only used if alpha still decodes to exactly 255. Measured on the 4K test
  frame: alpha min 255 at every quality, PSNR unchanged (Normal 42.16 dB, High 42.19 dB).
  Fast output is byte-identical to 1.1.4.
- pphap no longer writes `PPHAP-log.txt` into the current folder (an empty log path now really
  means "no log").
- pphap: a source frame rate above 240 fps (a container clock such as 1000/1) is replaced by the
  measured average rate; the frame count no longer includes frames a source's edit list hides,
  so `percent` reaches 100.
- pphap on Windows: Ctrl+Break (`CTRL_BREAK_EVENT`) now stops cleanly like Ctrl+C (file finalised,
  exit code 255) instead of killing the process.

## 1.1.4 — 2026-10-07
Licensing / public-release housekeeping. No encoder changes: output is byte-identical to 1.1.3.
- license.txt: new header explaining where the source code is (LGPL), plus missing notices for
  rgbcx (MIT), nlohmann/json (MIT), StackWalker (BSD-2, Windows) and the full Apache 2.0 text
  for bc7e. Old FFmpeg 4.0 wording corrected.
- Installers now also install the licences as PixelPressHAP-Licenses.txt (Mac:
  /Library/Application Support/PixelPressHAP, Windows: install folder).
- FFmpeg pinned to tag n4.4.8 (was the moving release/4.4 branch) so the published source
  matches the binaries exactly.
- scripts/make-source-release.sh: builds the public source zips (no Adobe SDKs) that ship
  next to each installer.

## 1.1.3 — 2026-09-29
Hap Q colour-accuracy fix.
- Hap Q / Hap Q Alpha: the RGB -> YCoCg conversion now rounds instead of truncating. The old
  conversion (inherited from the original plugin, also in 1.0) left decoded colours shifted on
  average (blue ~+0.45, green ~-0.53 levels). Shift now ~0.1 level or less; RGB PSNR +0.4 dB,
  luma +1.3 dB, mean ΔE2000 -19 to -23% (1080p test set, all quality settings). No speed cost.
- Hap, Hap Alpha, Alpha-Only and Hap R output is byte-identical to 1.1.2; `-legacy` /
  PPHAP_ENGINE=legacy is still byte-identical to 1.0.0.

## 1.1.2 — 2026-09-29
Hap Q encodes faster with byte-identical output (verified against 1.1.1 on test clips and the
stress-test worst cases, at Fast / Normal / High).
- Hap Q colour search: explicit SIMD (NEON on Apple Silicon, SSE2 on Windows), a per-block cache
  of scored endpoints, candidates abandoned as soon as they can't win, exact exits on zero error.
  Encoding work: Fast -45%, Normal -40%, High -30%.
- Mac and Windows builds now produce bit-identical files (the compiler may no longer fuse the
  Hap Q error maths into FMA instructions on Apple Silicon; 1.1.1 on Mac differed from Windows in
  ~0.03% of blocks).

## 1.1.1 — 2026-09-29
- Fix: the build failed on Mac and Windows (pphap could not find codec/texture_hq.hpp). No other changes from 1.1.0, which never produced installers.

## 1.1.0 — 2026-09-29
Colour accuracy release. 1.0.0 output stays available (`pphap -legacy`, `PPHAP_ENGINE=legacy`).
- pphap: YUV→RGB conversion uses the source's matrix / range tags, full-precision rounding and
  chroma (was ffmpeg's default fast path). New `-colorspace`, `-color_range`, `-colorconv`.
- Hap / Hap Alpha: rgbcx BC1 / BC3 encoder (was squish). Fast +2.2 dB at the same speed.
- Hap Q: high-quality Y (BC4) and least-squares Co/Cg encoder (was the real-time encoder).
- Alpha planes: high-quality BC4.
- Fast / Normal / High for every format.
- `pphap -psnr`: decodes every encoded frame and reports PSNR and CIEDE2000.
- `pphap -engine legacy`, `-legacy`: the 1.0.0 encoders, byte-identical output.
- Encoder stress test now covers every texture format, not just Hap R.
- CI picks the newest versioned project zip (PixelPressHAP-x.y.z.zip).

## 1.0.0 — 2026-09-24
First release: Hap R for Adobe CC on Apple Silicon and Windows, pphap command-line encoder.
