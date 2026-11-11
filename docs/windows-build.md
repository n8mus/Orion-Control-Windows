# Building Orion Control on Windows

This repository is the standalone Windows console. The app is Qt6/C++20,
built with MSVC; its platform boundaries (serial transport, audio I/O, the
crash handler, helper programs) sit behind `#ifdef Q_OS_WIN`. The Linux code
paths are still in the source, dormant: do not edit them out, add the Windows
path beside them.

Everything below is 64-bit. The paths shown are the ones on the station's
build PC; anything under `C:\Users\jon55\third_party` can live wherever you
like as long as the configure line points at it.

---

## 1. Prerequisites (fresh box, in this order)

### Visual Studio 2022 Build Tools (MSVC)
Download **"Build Tools for Visual Studio 2022"** (or Visual Studio 2022
Community) from <https://visualstudio.microsoft.com/downloads/> and install
the workload **Desktop development with C++**. That one workload gives you
`cl.exe`, the Windows SDK and bundled **CMake + Ninja**.

Every build command below runs from the **"x64 Native Tools Command Prompt
for VS 2022"**, which is what puts `cl`, `cmake` and `ninja` on the path.

### Qt 6.8 for Windows (MSVC 2022 64-bit)
Either the Qt Online Installer, or `aqtinstall`, which needs no account:

```bat
python -m pip install aqtinstall
python -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 ^
  -m qtmultimedia qtserialport qtwebsockets qtshadertools ^
  --outputdir C:\Users\jon55\third_party\qt-latest
```

All four add-on modules are needed: **Serial Port** (radio, wattmeters,
rotor), **Multimedia** (every audio path on Windows), **WebSockets** (FreeDV
Reporter, Winlink monitor) and **Shader Tools** (the GPU panadapter).

### SDRplay API for Windows (the one proprietary piece)
Install the Windows API/HW driver from <https://www.sdrplay.com/api/>. It
lands in `C:\Program Files\SDRplay\API\` (`inc\`, `x64\sdrplay_api.lib`,
`x64\sdrplay_api.dll`, and the `sdrplay_apiService` Windows service).
**Never bundle its files in a release.** The exe delay-loads the DLL and
finds the operator's own install at run time.

### rnnoise 0.2 (hard link dependency)
The vcpkg port does not build on MSVC. Build the xiph 0.2 release tarball
(<https://github.com/xiph/rnnoise/releases/download/v0.2/rnnoise-0.2.tar.gz>)
with this `CMakeLists.txt` dropped into its root:

```cmake
cmake_minimum_required(VERSION 3.20)
project(rnnoise C)
add_library(rnnoise STATIC
  src/denoise.c src/rnn.c src/pitch.c src/kiss_fft.c src/celt_lpc.c
  src/nnet.c src/nnet_default.c src/parse_lpcnet_weights.c
  src/rnnoise_data.c src/rnnoise_tables.c)
target_include_directories(rnnoise PUBLIC include PRIVATE src)
# MSVC never predefines __SSE2__; without it vec.h wants a header the
# release does not ship.
target_compile_definitions(rnnoise PRIVATE __SSE2__ RNNOISE_BUILD)
install(TARGETS rnnoise ARCHIVE DESTINATION lib)
install(FILES include/rnnoise.h DESTINATION include)
```

```bat
cmake -S rnnoise-0.2 -B rnnoise-build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_INSTALL_PREFIX=C:\Users\jon55\third_party\rnnoise-install
cmake --build rnnoise-build && cmake --install rnnoise-build
```

### FFTW 3.3.10, double precision, DLL (SDR receive audio / WDSP)
From <https://www.fftw.org/fftw-3.3.10.tar.gz>. **Apply
`packaging/windows/fftw-3.3.10-msvc-portable.patch` first** (`patch -p0` in
the extracted folder). Upstream's CMakeLists puts `/arch:AVX2` on the whole
library, so the planner itself comes out AVX2-only and the DLL dies with an
illegal instruction on an older CPU. The patch puts each instruction-set
flag only on that set's codelets, the way FFTW's own autotools build does;
FFTW still picks AVX/AVX2 at run time.

```bat
cmake -S fftw-3.3.10 -B fftw-3.3.10\build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DBUILD_SHARED_LIBS=ON -DBUILD_TESTS=OFF -DENABLE_SSE2=ON -DENABLE_AVX=ON ^
  -DENABLE_AVX2=ON -DENABLE_THREADS=OFF ^
  -DCMAKE_INSTALL_PREFIX=C:\Users\jon55\third_party\fftw-install
cmake --build fftw-3.3.10\build -j 8 && cmake --install fftw-3.3.10\build
```

Result: `include\fftw3.h`, `lib\fftw3.lib`, `bin\fftw3.dll`.

### whisper.cpp 1.9.5 (Copy Assist)

```bat
git clone --depth 1 -b v1.9.5 https://github.com/ggml-org/whisper.cpp
cmake -S whisper.cpp -B whisper.cpp\build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DBUILD_SHARED_LIBS=ON -DGGML_NATIVE=OFF -DGGML_BACKEND_DL=ON ^
  -DGGML_CPU_ALL_VARIANTS=ON -DGGML_OPENMP=OFF -DWHISPER_BUILD_TESTS=OFF ^
  -DWHISPER_BUILD_EXAMPLES=OFF -DWHISPER_BUILD_SERVER=OFF ^
  -DCMAKE_INSTALL_PREFIX=C:\Users\jon55\third_party\whisper-install
cmake --build whisper.cpp\build -j 8 && cmake --install whisper.cpp\build
```

`GGML_NATIVE=OFF` with `GGML_BACKEND_DL` + `GGML_CPU_ALL_VARIANTS` is the
point: the binary is not tuned to the build PC. It ships one
`ggml-cpu-<variant>.dll` per CPU generation and the console
(`ggml_backend_load_all()`) picks the best one the user's CPU can run.

The speech model is `ggml-base.en.bin` (148 MB, MIT) from
<https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.en.bin>.
The release zip carries it in `models\`; an operator's own copy in
`%LOCALAPPDATA%\n8mus\tentec-console\models\` wins over the bundled one.

### RADE (FreeDV digital voice)
`third_party/rade/windows/README.md`: two scripts fetch the pinned rade_c and
Opus sources and build them as static MSVC libraries.

### VOACAP engine (voacapl.exe + itshfbc)
voacapl is Fortran, which MSVC cannot build, so this needs MSYS2's MinGW
gfortran (`winget install MSYS2.MSYS2`, then
`pacman -S --needed mingw-w64-x86_64-gcc-fortran autoconf automake libtool make git`).
From the **MINGW64** shell:

```sh
git clone --depth 1 https://github.com/jawatson/voacapl /tmp/voacapl
cd /tmp/voacapl && autoreconf -i
./configure --prefix=/tmp/voa LDFLAGS="-static"
make -j$(nproc)
# Upstream's hooks write $(DESTDIR)/$(bindir); with DESTDIR empty that is
# "//tmp/voa/bin", which MSYS resolves as a UNC host and the install dies
# mid-way (leaving NO coefficient files behind). Drop the stray slash:
sed -i 's|$(DESTDIR)/$(bindir)|$(DESTDIR)$(bindir)|g;
        s|$(DESTDIR)/$(datadir)|$(DESTDIR)$(datadir)|g' Makefile
make install
HOME=/tmp/voahome /tmp/voa/bin/makeitshfbc
DEST=/c/Users/jon55/third_party/voacapl-win
mkdir -p $DEST && cp /tmp/voa/bin/voacapl.exe $DEST/
cp -rL /tmp/voahome/itshfbc $DEST/itshfbc      # -L: never ship symlinks
```

`ls $DEST/itshfbc/coeffs | wc -l` must print **45**; the release script
enforces that. The ITSHFBC tree an **ICEPAC for Windows** installer drops in
`C:\itshfbc` is *not* interchangeable (it carries only the `*W.BIN`
coefficient variants), so the console never looks there: it uses the bundled
tree, seeded into `%APPDATA%\n8mus\tentec-console\itshfbc` on first run
because voacapl writes its decks into `<root>\run`.

---

## 2. Build

From the x64 Native Tools prompt, in the repo root:

```bat
cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
  -DCMAKE_PREFIX_PATH="C:/Users/jon55/third_party/qt-latest/6.8.3/msvc2022_64;C:/Users/jon55/third_party/rnnoise-install;C:/Users/jon55/third_party/fftw-install;C:/Users/jon55/third_party/whisper-install" ^
  -DRADE_WIN_ROOT=C:/Users/jon55/third_party/rade-win/install ^
  -DBUILD_SDRPLAY=ON
cmake --build build-win -j 8
```

**`-DCMAKE_BUILD_TYPE=RelWithDebInfo` is not optional.** VS's bundled CMake
initializes the build type to **Debug**, and a Debug build cannot keep up
with the 1 MHz IQ stream.

A complete configure prints all five of these; a missing one means that
feature was silently left out:

```
-- RADE: prebuilt from ...
-- WDSP: SDR receive audio enabled
-- GPU panadapter: on (Qt 6.8.3)
-- Copy Assist: whisper 1.9.5... found
-- SDRplay RSP source: ENABLED
```

To run from the build folder, the runtime must sit beside the exe:

```bat
C:\Users\jon55\third_party\qt-latest\6.8.3\msvc2022_64\bin\windeployqt.exe ^
  --release --no-translations --compiler-runtime build-win\tentec-console.exe
copy C:\Users\jon55\third_party\fftw-install\bin\fftw3.dll build-win\
copy C:\Users\jon55\third_party\whisper-install\bin\whisper.dll build-win\
copy C:\Users\jon55\third_party\whisper-install\bin\ggml*.dll build-win\
mkdir build-win\models
copy C:\Users\jon55\third_party\whisper-models\ggml-base.en.bin build-win\models\
copy C:\Users\jon55\third_party\voacapl-win\voacapl.exe build-win\
xcopy /E /I C:\Users\jon55\third_party\voacapl-win\itshfbc build-win\itshfbc
```

A missing DLL shows as an instant silent exit (it is a GUI-subsystem exe).
After linking a new Qt module, run windeployqt again.

---

## 3. Test

No test keys a transmitter, and none may. Build the test targets explicitly
(they are `EXCLUDE_FROM_ALL`), then run them from `build-win`:

```bat
cmake --build build-win --target cwtest rttytest skimtest sdrrxtest ^
  raderxtest radetxtest asrtest winaudiotest contesttest logtest vkeqtest
```

Windows notes on the suite:

- **`winaudiotest`** is the Windows-only one: the Qt Multimedia endpoints
  that stand in for `pacat`/`parec` (`PcmSink` on the GUI thread and on its
  own, and the LIVE mic chain). It uses the PC's default devices only, plays
  a -60 dBFS tone, and never names the radio's sound device.
- **`cwtest`** full matrix on MSVC (2026-11-11): engine 96%, legacy 97%.
  The legacy decoder's *dead channel* row reads 46 garbage characters against
  a bar of 45: the test's noise comes from `std::normal_distribution`, whose
  output differs between standard libraries (the test's own comment records
  32 on Arch and 36 on Ubuntu from the same seed). The engine, which is the
  default decoder everywhere, reads 37 and passes.
- **`wktest`** drives a POSIX pty and does not build on Windows.
- `QSettings` is the registry here, so `XDG_CONFIG_HOME` cannot give a test a
  clean configuration. **`TTC_PROFILE=<name>`** does: the console then uses
  `HKCU\Software\n8mus\tentec-console-<name>` and a matching data folder, and
  leaves the operator's own settings and logbook alone.

Headless self-test (also releases the SDR cleanly on exit). The offscreen
platform plugin and fonts are not staged by windeployqt: copy
`plugins\platforms\qoffscreen.dll` from the Qt install into
`build-win\platforms\`, and the DejaVu Sans / DejaVu Sans Mono `.ttf` files
into `build-win\lib\fonts\`. Then, in PowerShell:

```powershell
$env:QT_QPA_PLATFORM = "offscreen"; $env:TTC_SELFTEST = "8"
$env:TTC_RIGCTLD_PORT = "14599"; $env:TTC_RADIO = "orion"
$env:TTC_RADIO_DEV = "NUL"; $env:TTC_PROFILE = "selftest"
Start-Process build-win\tentec-console.exe -WorkingDirectory build-win `
  -RedirectStandardError err.txt -RedirectStandardOutput out.txt -Wait
```

`err.txt` ends with `[layout] window WxH layout-min WxH`; the width budget is
**layout-min <= 1850**. For a windowed run, do not redirect the streams (the
exe exits at once); set `TTC_LOG=<file>` instead and stderr goes there.
`TTC_UISCRIPT=<file>` drives menus and buttons and grabs screenshots
(`src/app/UiScript.h`); `TTC_TRACE=1` logs every CAT command sent, which is
how to prove a run only ever *queried* the radio.

---

## 4. Release

Releases are cut on the station's own Windows PC, never by CI:

```bat
git tag v0.2.0-2026.11.11
powershell -ExecutionPolicy Bypass -File packaging\make-windows-release.ps1
```

The script builds from a clean tree at the tag, refuses a zip that is
missing any feature, stages the runtime, licenses and README, hard-stops if
any `sdrplay_api*` file lands in the stage, and writes the zip plus its
SHA-256. With `-Draft` it attaches the zip to a new **draft** release for a
person to review and publish.

**A published release is never replaced or removed.** The script will not
upload to a tag that already has a release. A new build gets a new tag, whose
name ends in its build date, and is published beside the older downloads.
