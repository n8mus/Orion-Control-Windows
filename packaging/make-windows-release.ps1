# Cut the Windows release zip on the station's Win box and attach it to the
# GitHub Release for the current tag. Counterpart of the AppImage workflow
# (.github/workflows/release.yml): CI cuts Linux, this box cuts Windows —
# deliberately, so every shipped exe was built (and flown) where the radio is.
#
# Run from the "x64 Native Tools Command Prompt for VS 2022" (or any shell
# where cl/cmake/ninja resolve), in the repo root, with HEAD sitting exactly
# on the release tag:
#
#     powershell -ExecutionPolicy Bypass -File packaging\make-windows-release.ps1
#
# Same licensing rule as the AppImage: the proprietary sdrplay_api.dll is
# NEVER in the zip — the operator's own SDRplay API install provides it at
# runtime. The zip carries a README that says so.

param(
    # Qt and rnnoise installs, as documented in docs/windows-build.md.
    [string]$QtDir      = "C:\Qt\6.8.3\msvc2022_64",
    [string]$RnnoiseDir = "C:\Users\jon55\third_party\rnnoise-install",
    # Build with the SDR source (needs the SDRplay API SDK installed).
    [bool]$Sdrplay      = $true,
    # Build + zip but skip the GitHub upload (dry run / no network).
    [switch]$NoUpload,
    # Skip the clean-tree and exact-tag gates. For experiments only —
    # a shipped zip should always come from a pushed tag.
    [switch]$Force
)

$ErrorActionPreference = "Stop"

function Fail([string]$msg) { Write-Host "RELEASE: $msg" -ForegroundColor Red; exit 1 }
function Step([string]$msg) { Write-Host "`n=== $msg" -ForegroundColor Cyan }

# ---- Preflight --------------------------------------------------------------
Step "Preflight"
if (-not (Test-Path "CMakeLists.txt")) { Fail "run from the repo root" }
foreach ($tool in "git", "cmake", "ninja") {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        Fail "$tool not found - use the x64 Native Tools prompt"
    }
}
$windeployqt = Join-Path $QtDir "bin\windeployqt.exe"
if (-not (Test-Path $windeployqt)) {
    Fail "windeployqt not at $windeployqt - pass -QtDir for your Qt install"
}

$dirty = git status --porcelain
if ($dirty -and -not $Force) { Fail "working tree not clean - a release builds committed code" }

# Probe under EAP=Continue: with EAP=Stop, PS 5.1 turns a native command's
# redirected stderr into a terminating error, and git describe SPEAKS on
# stderr whenever HEAD isn't tagged - the very case being probed.
$ErrorActionPreference = "Continue"
$tag = git describe --tags --exact-match 2>$null
$ErrorActionPreference = "Stop"
if ($LASTEXITCODE -ne 0) { $tag = $null }
if (-not $tag) {
    if (-not $Force) { Fail "HEAD is not on a tag - releases are cut from vX.Y.Z tags (git tag / git push --tags first)" }
    $tag = "untagged-" + (git rev-parse --short HEAD)
}
Write-Host "tag: $tag"

# ---- Build ------------------------------------------------------------------
# Fresh dir every time: the dev build-win keeps its cache, the release
# never inherits one. RelWithDebInfo is explicit because VS's CMake fork
# force-defaults to Debug, and a Debug build cannot keep up with the IQ
# stream (docs/windows-build.md, learned the hard way on Linux first).
Step "Configure + build (RelWithDebInfo, SDRplay=$Sdrplay)"
$bld = "build-win-release"
if (Test-Path $bld) { Remove-Item -Recurse -Force $bld }
$sdrFlag = if ($Sdrplay) { "ON" } else { "OFF" }
cmake -B $bld -G Ninja `
    -DCMAKE_BUILD_TYPE=RelWithDebInfo `
    -DCMAKE_PREFIX_PATH="$QtDir;$RnnoiseDir" `
    -DBUILD_SDRPLAY=$sdrFlag
cmake --build $bld -j
if (-not (Test-Path "$bld\tentec-console.exe")) { Fail "build produced no exe" }

# ---- Stage ------------------------------------------------------------------
Step "Stage + windeployqt"
$name  = "tentec-console-$tag-win64"
$stage = Join-Path $bld $name
New-Item -ItemType Directory -Path $stage | Out-Null
Copy-Item "$bld\tentec-console.exe" $stage

# Qt runtime beside the exe. Plugins ride in from the exe's own imports
# (SerialPort, Multimedia); translations we don't have.
& $windeployqt --release --no-translations (Join-Path $stage "tentec-console.exe")
if ($LASTEXITCODE -ne 0) { Fail "windeployqt failed" }

# The one non-negotiable: the proprietary SDRplay runtime must not ship.
# windeployqt only stages Qt, but belt and braces - if it ever lands in
# the stage by any route, the release stops rather than ships it.
$contraband = Get-ChildItem -Recurse $stage -Filter "sdrplay_api*"
if ($contraband) { Fail "sdrplay_api files in the stage - never bundle them: $($contraband.Name)" }

@"
Orion Control $tag - Windows x64
================================

GPL-3 open source: https://github.com/n8mus/Orion-Control
(this zip is the corresponding binary; source for this exact build is
the $tag tag)

SDRplay panadapter: this build uses the SDRplay API but does NOT bundle
it (not redistributable). Install the SDRplay API/HW driver from
https://www.sdrplay.com/api/ - the console finds the installed API by
itself. Without it the console runs radio-only (no panadapter).

Run tentec-console.exe. Settings live per-user in the registry
(HKCU\Software\n8mus\tentec-console).
"@ | Set-Content (Join-Path $stage "README-WINDOWS.txt")

# ---- Zip + upload -----------------------------------------------------------
Step "Zip"
$zip = Join-Path $bld "$name.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path $stage -DestinationPath $zip
Write-Host "built: $zip ($([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB)"

if ($NoUpload) { Write-Host "`n-NoUpload set - stopping before GitHub"; exit 0 }

Step "Upload to GitHub Release $tag"
if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
    Fail "gh CLI not found - install GitHub CLI (winget install GitHub.cli) or rerun with -NoUpload and attach $zip by hand"
}
# The AppImage workflow creates the release when the tag is pushed; if this
# box gets there first, create it and the workflow attaches alongside.
# (Same EAP dance as git describe: "no such release" arrives on stderr.)
$ErrorActionPreference = "Continue"
gh release view $tag *> $null
$ErrorActionPreference = "Stop"
if ($LASTEXITCODE -ne 0) {
    gh release create $tag --title $tag --notes "Orion Control $tag"
    if ($LASTEXITCODE -ne 0) { Fail "could not create release $tag" }
}
gh release upload $tag $zip --clobber
if ($LASTEXITCODE -ne 0) { Fail "upload failed - zip is still at $zip" }
Write-Host "`nRELEASE: $name.zip attached to $tag" -ForegroundColor Green
