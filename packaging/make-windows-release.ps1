# Cut the Windows release zip on the station's Windows box.
#
# This repository is the standalone Windows console (github.com/n8mus/
# Orion-Control-Windows). Every shipped exe is built, and flown, where the
# radio is; nothing here is built by CI.
#
# Run from the "x64 Native Tools Command Prompt for VS 2022" (or any shell
# where cl/cmake/ninja resolve), in the repo root, with HEAD sitting exactly
# on the release tag:
#
#     powershell -ExecutionPolicy Bypass -File packaging\make-windows-release.ps1
#
# The version IS the tag, and the tag carries the build date:
# v<major>.<minor>.<patch>-<yyyy>.<mm>.<dd>  (for example v0.2.0-2026.11.11).
#
# TWO RULES THIS SCRIPT ENFORCES
#   1. The proprietary sdrplay_api.dll is NEVER in the zip. The operator's own
#      SDRplay API install provides it at run time.
#   2. A published release is NEVER replaced. If a release for this tag
#      already exists the script stops; it does not overwrite assets. A new
#      build gets a new tag and sits BESIDE the older downloads.
#
# With -Draft the zip is attached to a new DRAFT release for a person to
# review and publish. Without it the script stops after building the zip.

param(
    # Installs, as documented in docs/windows-build.md.
    [string]$QtDir        = "C:\Users\jon55\third_party\qt-latest\6.8.3\msvc2022_64",
    [string]$RnnoiseDir   = "C:\Users\jon55\third_party\rnnoise-install",
    [string]$FftwDir      = "C:\Users\jon55\third_party\fftw-install",
    [string]$WhisperDir   = "C:\Users\jon55\third_party\whisper-install",
    [string]$WhisperModel = "C:\Users\jon55\third_party\whisper-models\ggml-base.en.bin",
    [string]$RadeDir      = "C:\Users\jon55\third_party\rade-win\install",
    # voacapl.exe (statically linked, no MinGW DLLs) plus its itshfbc data
    # tree. Public-domain NTIA/ITS core, so bundling is legal and deliberate.
    [string]$VoacapDir    = "C:\Users\jon55\third_party\voacapl-win",
    [string]$Repo         = "n8mus/Orion-Control-Windows",
    # Build with the SDR source (needs the SDRplay API SDK installed).
    [bool]$Sdrplay        = $true,
    # Attach the zip to a new DRAFT release on GitHub (never to an existing one).
    [switch]$Draft,
    # Skip the clean-tree and exact-tag gates. For experiments only: a shipped
    # zip always comes from a tag.
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
# Fail here rather than ship a zip with a dead feature: that is exactly how a
# Windows build once went out without a VOACAP engine at all.
$need = @{
    "voacapl.exe"            = (Join-Path $VoacapDir "voacapl.exe")
    "itshfbc data tree"      = (Join-Path $VoacapDir "itshfbc\run")
    "fftw3.dll (WDSP)"       = (Join-Path $FftwDir "bin\fftw3.dll")
    "whisper.dll"            = (Join-Path $WhisperDir "bin\whisper.dll")
    "speech model"           = $WhisperModel
    "RADE libraries"         = (Join-Path $RadeDir "lib\rade.lib")
}
foreach ($k in $need.Keys) {
    if (-not (Test-Path $need[$k])) { Fail "$k not at $($need[$k]) - see docs/windows-build.md" }
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
    if (-not $Force) { Fail "HEAD is not on a tag - releases are cut from tags like v0.2.0-2026.11.11" }
    $tag = "untagged-" + (git rev-parse --short HEAD)
}
if ($tag -notmatch '\d{4}\.\d{2}\.\d{2}$' -and -not $Force) {
    Fail "tag '$tag' does not end in the build date (yyyy.mm.dd) - the version must say when it was built"
}
Write-Host "tag: $tag"

# ---- Build ------------------------------------------------------------------
# Fresh dir every time: the dev build-win keeps its cache, the release never
# inherits one. RelWithDebInfo is explicit because VS's CMake fork force-
# defaults to Debug, and a Debug build cannot keep up with the IQ stream.
Step "Configure + build (RelWithDebInfo, SDRplay=$Sdrplay)"
$bld = "build-win-release"
if (Test-Path $bld) { Remove-Item -Recurse -Force $bld }
$sdrFlag = if ($Sdrplay) { "ON" } else { "OFF" }
$prefix = (@($QtDir, $RnnoiseDir, $FftwDir, $WhisperDir) | ForEach-Object { $_ -replace '\\', '/' }) -join ";"
$radeRoot = $RadeDir -replace '\\', '/'
New-Item -ItemType Directory -Path $bld | Out-Null
$cfgLog = Join-Path $bld "configure.log"
# EAP=Continue around the redirect: with EAP=Stop, PS 5.1 turns a native
# command's redirected stderr into a terminating error, and CMake prints its
# warnings there (the optional Qt6GuiPrivate package always earns one).
$ErrorActionPreference = "Continue"
cmake -B $bld -G Ninja `
    -DCMAKE_BUILD_TYPE=RelWithDebInfo `
    "-DCMAKE_PREFIX_PATH=$prefix" `
    "-DRADE_WIN_ROOT=$radeRoot" `
    -DBUILD_SDRPLAY=$sdrFlag 2>&1 | ForEach-Object { "$_" } | Tee-Object $cfgLog
$cfgExit = $LASTEXITCODE
$ErrorActionPreference = "Stop"
if ($cfgExit -ne 0) { Fail "configure failed" }
# The published build is the whole console. A zip that quietly fell back to a
# lesser one is a bug, so stop instead.
foreach ($must in "WDSP: SDR receive audio enabled", "RADE: prebuilt from",
                  "Copy Assist: whisper", "GPU panadapter: on") {
    if (-not (Select-String -Quiet -SimpleMatch -Path $cfgLog -Pattern $must)) {
        Fail "configure did not report '$must' - not shipping a lesser build"
    }
}
if ($Sdrplay -and -not (Select-String -Quiet -SimpleMatch -Path $cfgLog -Pattern "SDRplay RSP source: ENABLED")) {
    Fail "SDRplay source not enabled"
}
cmake --build $bld -j 8
if ($LASTEXITCODE -ne 0) { Fail "build failed" }
if (-not (Test-Path "$bld\tentec-console.exe")) { Fail "build produced no exe" }

# ---- Stage ------------------------------------------------------------------
Step "Stage + windeployqt"
$name  = "tentec-console-$tag-win64"
$stage = Join-Path $bld $name
New-Item -ItemType Directory -Path $stage | Out-Null
Copy-Item "$bld\tentec-console.exe" $stage

# Qt runtime beside the exe, plus the VC++ redistributable installer for a PC
# that has never had it.
& $windeployqt --release --no-translations --compiler-runtime (Join-Path $stage "tentec-console.exe")
if ($LASTEXITCODE -ne 0) { Fail "windeployqt failed" }

Step "Stage WDSP (FFTW), Copy Assist (whisper.cpp + model)"
Copy-Item (Join-Path $FftwDir "bin\fftw3.dll") $stage
# whisper.dll does not load its CPU backends itself: the console calls
# ggml_backend_load_all(), which scans the exe's folder for ggml-cpu-*.dll
# and picks the best one this CPU can run. All of them ship.
foreach ($pat in "whisper.dll", "ggml.dll", "ggml-base.dll", "ggml-cpu-*.dll") {
    Copy-Item (Join-Path $WhisperDir "bin\$pat") $stage
}
$cpu = (Get-ChildItem $stage -Filter "ggml-cpu-*.dll").Count
if ($cpu -lt 4) { Fail "only $cpu ggml CPU backends staged - Copy Assist would not run on other PCs" }
New-Item -ItemType Directory -Path (Join-Path $stage "models") | Out-Null
Copy-Item $WhisperModel (Join-Path $stage "models\ggml-base.en.bin")

# VOACAP engine + data, flat beside the exe: Voacap.cpp looks for
# <exe>/voacapl.exe and seeds a writable copy of <exe>/itshfbc into AppData
# on first use.
Step "Stage VOACAP engine"
Copy-Item (Join-Path $VoacapDir "voacapl.exe") $stage
Copy-Item (Join-Path $VoacapDir "itshfbc") (Join-Path $stage "itshfbc") -Recurse
$coeffs = (Get-ChildItem (Join-Path $stage "itshfbc\coeffs") -File).Count
if ($coeffs -lt 40) { Fail "itshfbc/coeffs has only $coeffs files - expected 45" }

Step "Stage licenses"
$lic = Join-Path $stage "licenses"
New-Item -ItemType Directory -Path $lic | Out-Null
Copy-Item "LICENSE" (Join-Path $lic "Orion-Control-GPL-3.txt")
Copy-Item "NOTICE" (Join-Path $lic "Orion-Control-NOTICE.txt")
Copy-Item "third_party\wdsp\LICENSE" (Join-Path $lic "WDSP-GPL-2.txt")
Copy-Item "packaging\third-party-licenses-windows.txt" (Join-Path $lic "THIRD-PARTY.txt")
if (Test-Path (Join-Path $RadeDir "licenses")) {
    Copy-Item (Join-Path $RadeDir "licenses\*") $lic
}

# The one non-negotiable: the proprietary SDRplay runtime must not ship.
$contraband = Get-ChildItem -Recurse $stage -Filter "sdrplay_api*"
if ($contraband) { Fail "sdrplay_api files in the stage - never bundle them: $($contraband.Name)" }

$built = Get-Date -Format "yyyy-MM-dd"
@"
Orion Control for Windows $tag  (built $built)
==========================================================

Source (GPL-3): https://github.com/$Repo
This zip is the binary for the tag $tag of that repository.

BEFORE YOU RUN IT
  Install the SDRplay API / hardware driver from https://www.sdrplay.com/api/
  It is proprietary and is not in this zip. The console finds your install by
  itself. Without it the console still runs the radio, with a dark panadapter.

RUN
  Unzip the WHOLE folder anywhere and run tentec-console.exe inside it. The
  files beside it are needed. If Windows says a runtime DLL is missing, run
  vc_redist.x64.exe (in this folder) once.
  First launch opens Setup > Station: pick the radio's COM port there.
  Settings are stored per user in the registry
  (HKCU\Software\n8mus\tentec-console).

IN THIS FOLDER
  fftw3.dll               receive audio from the SDR (WDSP)
  whisper.dll, ggml*.dll  Copy Assist (speech to text); models\ holds its
                          English speech model
  voacapl.exe, itshfbc\   VOACAP propagation overlay (public-domain engine).
                          The console copies the data into your AppData on
                          first use.
  licenses\               the licenses of everything bundled

FreeDV RADE digital voice is built into tentec-console.exe.

Older Windows builds stay available on the Releases page of the repository.
"@ | Set-Content (Join-Path $stage "README-WINDOWS.txt")

# ---- Zip --------------------------------------------------------------------
Step "Zip"
$zip = Join-Path $bld "$name.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path $stage -DestinationPath $zip
$sha = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
Write-Host "built: $zip ($([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB)"
Write-Host "sha256: $sha"

if (-not $Draft) { Write-Host "`nNo -Draft: stopping before GitHub. The zip is ready."; exit 0 }

# ---- Draft release ----------------------------------------------------------
Step "Draft release $tag on $Repo"
if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
    Fail "gh CLI not found - attach $zip by hand"
}
$ErrorActionPreference = "Continue"
gh release view $tag -R $Repo *> $null
$exists = ($LASTEXITCODE -eq 0)
$ErrorActionPreference = "Stop"
if ($exists) {
    Fail "a release for $tag already exists on $Repo. Published releases are never replaced: cut a new tag instead."
}
gh release create $tag $zip -R $Repo --draft --verify-tag `
    --title "$tag, built $built" `
    --notes "Orion Control for Windows $tag, built $built. SHA-256 of the zip: ``$sha``"
if ($LASTEXITCODE -ne 0) { Fail "could not create the draft release - the zip is still at $zip" }
Write-Host "`nRELEASE: draft created for $tag with $name.zip. Review and publish it by hand." -ForegroundColor Green
