# Builds PixelPress HAP for Windows x64 and packages an installer (.exe, NSIS).
#
# Needs: Visual Studio 2022 with "Desktop development with C++" (includes CMake),
#        Git, NSIS (https://nsis.sourceforge.io), MSYS2 (https://www.msys2.org) for the
#        one-off FFmpeg build, and the Adobe SDKs (run scripts\get_adobe_sdk.py first).
# Run from a normal PowerShell:  powershell -ExecutionPolicy Bypass -File scripts\build-win.ps1
$ErrorActionPreference = 'Stop'
$Root = Split-Path $PSScriptRoot -Parent
function Fail($m) { Write-Host "`nERROR: $m" -ForegroundColor Red; exit 1 }

# 1.2.3: the project may live in a folder with spaces (e.g. C:\Projects\Pixel Press Plugin\Source Code\
# PixelPressHAP), which FFmpeg's build can't handle. Then the build runs through a temporary drive
# letter (subst P: = the project folder) and the letter is removed afterwards.
if ($Root -match ' ' -and -not $env:PPHAP_BUILD_SUBST) {
    $letter = @('P','Q','R','S','T','U','V','W','Y','Z') | Where-Object { -not (Test-Path "${_}:\") } | Select-Object -First 1
    if (-not $letter) { Fail "no free drive letter for the build (the project path has spaces)" }
    subst "${letter}:" "$Root"
    if ($LASTEXITCODE) { Fail "subst ${letter}: failed" }
    Write-Host "Project folder has spaces - building through drive ${letter}: (removed again afterwards)"
    $env:PPHAP_BUILD_SUBST = '1'
    $env:PPHAP_REAL_ROOT = $Root
    try { & powershell -NoProfile -ExecutionPolicy Bypass -File "${letter}:\scripts\build-win.ps1"; $code = $LASTEXITCODE }
    finally { subst "${letter}:" /d | Out-Null }
    if ($code) { exit $code }
    Write-Host "`nOK: the installer is in $Root\dist" -ForegroundColor Green
    exit 0
}
Set-Location $Root
# the real project folder (not the temporary drive letter), and the project folders around it when it
# sits in "Pixel Press Plugin\Source Code\" (same layout as the Mac / cloud backup)
$RealRoot = if ($env:PPHAP_REAL_ROOT) { $env:PPHAP_REAL_ROOT } else { $Root }
$PluginDir = Split-Path (Split-Path $RealRoot -Parent) -Parent
$Layout = (Split-Path (Split-Path $RealRoot -Parent) -Leaf) -eq 'Source Code'
# a build folder configured from another path (the project was moved, or a different drive letter):
# CMake refuses it, so start that folder fresh (FFmpeg and ISPC are kept)
if (Test-Path build-win\CMakeCache.txt) {
    $home_ = (Select-String -Path build-win\CMakeCache.txt -Pattern '^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$').Matches.Groups[1].Value
    if ($home_ -and ((Resolve-Path $home_ -ErrorAction SilentlyContinue).Path -ne (Get-Location).Path)) {
        Write-Host "build-win was set up for $home_ - starting it fresh"
        Remove-Item -Recurse -Force build-win
    }
}

$Adobe = "external\foundation\external\adobe"
if (-not (Test-Path "$Adobe\AfterEffectsSDK\Examples\Headers\AE_GeneralPlug.h")) { Fail "After Effects SDK missing - run: python scripts\get_adobe_sdk.py <AE SDK zip> <Premiere SDK zip>" }
if (-not (Test-Path "$Adobe\premiere\Headers\PrSDKExport.h")) { Fail "Premiere Pro SDK missing - run scripts\get_adobe_sdk.py" }

# --- dependencies -----------------------------------------------------------
function Clone($url, $ref, $dest) {
    if (-not (Test-Path "$dest\.fetched")) {
        if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
        Write-Host "Fetching $dest ..."
        git clone -q --depth 1 --branch $ref $url $dest; if ($LASTEXITCODE) { Fail "git clone $url failed" }
        New-Item "$dest\.fetched" -ItemType File | Out-Null
    }
}
$Ext = "external\foundation\external"
Clone https://github.com/FFmpeg/FFmpeg.git n4.4.8 "$Ext\ffmpeg\FFmpeg"
Clone https://github.com/nlohmann/json.git v3.11.3 "$Ext\json"
Clone https://github.com/JochenKalmbach/StackWalker.git master "$Ext\StackWalker"

# --- Visual Studio ------------------------------------------------------------
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { Fail "Visual Studio 2022 not found" }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { Fail "Visual Studio C++ tools not found (install 'Desktop development with C++')" }
$vcvars = "$vs\VC\Auxiliary\Build\vcvars64.bat"
# Load the Visual Studio x64 environment into this PowerShell (cl.exe, and the CMake / CPack that
# come with Visual Studio), so the script works from a normal PowerShell window every time.
cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process') }
}
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) { Fail "Could not load the Visual Studio compiler environment ($vcvars)" }
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) { Fail "CMake not found (install it, or add 'C++ CMake tools for Windows' in the Visual Studio Installer)" }
# ffmpeg.exe / ffprobe.exe for C:\Program Files\PixelPress: Pixel Press Plugin\3rd Party\ffmpeg-win, or dist\ffmpeg-win
$candidates = @("$Root\dist\ffmpeg-win")
if ($Layout) { $candidates = @("$PluginDir\3rd Party\ffmpeg-win") + $candidates }
$FfWin = $candidates | Where-Object { Test-Path "$_\ffmpeg.exe" } | Select-Object -First 1
if (-not $FfWin) { Fail "ffmpeg-win\ffmpeg.exe not found (Pixel Press Plugin\3rd Party\ffmpeg-win or dist\ffmpeg-win)" }

# --- FFmpeg (static, MSVC, QuickTime muxer only, LGPL) -----------------------
$ff = Resolve-Path "$Ext\ffmpeg\FFmpeg"
if (-not (Test-Path "$ff\libavformat\libavformat.a")) {
    $msys = if ($env:MSYS2_ROOT) { $env:MSYS2_ROOT } else { 'C:\msys64' }
    $bash = "$msys\usr\bin\bash.exe"
    if (-not (Test-Path $bash)) { Fail "MSYS2 not found at $msys (install from msys2.org or set MSYS2_ROOT)" }
    & $bash -lc "pacman -S --needed --noconfirm make diffutils >/dev/null"
    # MSYS2's /usr/bin/link.exe shadows the MSVC linker during FFmpeg's configure
    & $bash -lc "[ -f /usr/bin/link.exe ] && mv /usr/bin/link.exe /usr/bin/link.exe.msys || true"
    # MSYS2's bash inherits the Visual Studio environment loaded above (MSYS2_PATH_TYPE=inherit),
    # so FFmpeg's configure finds cl.exe.
    $env:MSYS2_PATH_TYPE = 'inherit'
    $env:CHERE_INVOKING = '1'
    $script = (Join-Path $PSScriptRoot 'build-ffmpeg-win.sh') -replace '\\','/'
    Write-Host "Building FFmpeg (one-off, a few minutes) ..."
    & $bash -l $script
    if (-not (Test-Path "$ff\libavformat\libavformat.a")) { Fail "FFmpeg build failed" }
}

# --- Intel ISPC compiler for the bc7e (Hap R) encoder -------------------------
$IspcVer = '1.25.3'
$IspcDir = Join-Path $Root ".tools\ispc-$IspcVer"
$Ispc = Join-Path $IspcDir 'bin\ispc.exe'
if (-not (Test-Path $Ispc)) {
    Write-Host "Downloading ISPC $IspcVer ..."
    New-Item -ItemType Directory -Force (Join-Path $Root '.tools') | Out-Null
    $zip = Join-Path $Root ".tools\ispc.zip"
    Invoke-WebRequest -UseBasicParsing "https://github.com/ispc/ispc/releases/download/v$IspcVer/ispc-v$IspcVer-windows.zip" -OutFile $zip
    $tmp = Join-Path $Root '.tools\ispc-extract'
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    Expand-Archive $zip -DestinationPath $tmp
    $found = Get-ChildItem -Recurse $tmp -Filter ispc.exe | Select-Object -First 1
    if (-not $found) { Fail "ispc.exe not found in the ISPC download" }
    if (Test-Path $IspcDir) { Remove-Item -Recurse -Force $IspcDir }
    Move-Item $found.Directory.Parent.FullName $IspcDir
    Remove-Item -Recurse -Force $tmp, $zip
}
& $Ispc --version

# --- plugin -------------------------------------------------------------------
# CMAKE_POLICY_VERSION_MINIMUM: CMake 4 refuses the old cmake_minimum_required() in bundled libraries
# (snappy). Quoted: PowerShell splits an unquoted -D...=3.5 at the dot.
cmake -S . -B build-win -G "Visual Studio 17 2022" -A x64 "-DISPC_EXECUTABLE=$Ispc" "-DCMAKE_POLICY_VERSION_MINIMUM=3.5" "-DPP_FFMPEG_WIN_DIR=$($FfWin -replace '\\','/')"
if ($LASTEXITCODE) { Fail "CMake configure failed" }
cmake --build build-win --config Release -- /m
if ($LASTEXITCODE) { Fail "Build failed" }

# --- installer ----------------------------------------------------------------
if (-not (Get-Command makensis -ErrorAction SilentlyContinue)) {
    $env:Path += ";${env:ProgramFiles(x86)}\NSIS;$env:ProgramFiles\NSIS"
}
if (-not (Get-Command makensis -ErrorAction SilentlyContinue)) { Fail "NSIS not found (install from nsis.sourceforge.io)" }
Push-Location build-win
cpack -G NSIS -C Release
if ($LASTEXITCODE) { Pop-Location; Fail "Packaging failed" }
$exe = Get-ChildItem PixelPressHAP-*.exe | Sort-Object LastWriteTime -Descending | Select-Object -First 1
Pop-Location
New-Item -ItemType Directory -Force dist | Out-Null
Copy-Item "build-win\$($exe.Name)" dist\
$cli = Get-ChildItem -Recurse build-win -Filter pphap.exe | Where-Object { $_.FullName -match 'Release' } | Select-Object -First 1
if (-not $cli) { Fail "pphap.exe command-line tool was not built" }
Copy-Item $cli.FullName dist\
Write-Host "`nOK: Installer: $RealRoot\dist\$($exe.Name)" -ForegroundColor Green
# 1.2.3: also file it under Pixel Press Plugin\Deployment\Releases\<version>\ (Mac / cloud layout)
if ($Layout -and $exe.Name -match 'PixelPressHAP-(\d+\.\d+\.\d+)') {
    $rel = "$PluginDir\Deployment\Releases\$($matches[1])"
    New-Item -ItemType Directory -Force $rel | Out-Null
    Copy-Item "build-win\$($exe.Name)" $rel
    Write-Host "    also in $rel" -ForegroundColor Green
}
