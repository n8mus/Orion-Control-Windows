# Building the console on Windows (native port — `windows-port` branch)

Phase 2: the Ten-Tec console runs natively on Windows, from the **same
codebase** as Linux. The app is Qt6/C++20; portability comes from Qt plus a
handful of platform boundaries (serial transport, audio I/O, the crash
handler) guarded by `#ifdef Q_OS_WIN` / `Q_OS_UNIX`. This file is the
from-scratch setup for a Windows build box and the eventual build command.

The Linux build (`README.md`, `CLAUDE.md`) is unchanged — do not edit the
Linux paths out; add the Windows path beside them.

---

## 0. Prerequisites — install these first (fresh box, in this order)

Everything below is 64-bit. Reboot when an installer asks.

### 1. Visual Studio 2022 — C++ toolchain  (MSVC)
Download **"Build Tools for Visual Studio 2022"** (or full Visual Studio 2022
Community) from <https://visualstudio.microsoft.com/downloads/>.
In the installer, check the workload:

- **Desktop development with C++**

That single workload gives you the MSVC compiler (`cl.exe`), the Windows SDK,
and bundled **CMake + Ninja**. MSVC is the recommended compiler because the
SDRplay API and most Windows libraries ship MSVC-built.

> Alternative: MinGW-w64 (Qt can install it) is lighter but links the
> proprietary SDRplay `.lib` less cleanly. Stick with MSVC unless we hit a
> reason not to.

### 2. Qt 6 for Windows  (MSVC build)
Get the **Qt Online Installer** from <https://www.qt.io/download-qt-installer>
(a free Qt account is required for the open-source edition).

When selecting components, pick the latest **Qt 6.x** (6.8 LTS or newer) and
under it the **MSVC 2022 64-bit** build. Then — this is the part the default
selection misses — expand **Additional Libraries** and check:

- **Qt Serial Port**   ← radios, wattmeters, DCU rotor
- **Qt Multimedia**    ← the Windows audio backend (replaces PipeWire CLI)

Core / Gui / Widgets / Network come with the base install. Note the install
path, e.g. `C:\Qt\6.8.1\msvc2022_64` — CMake needs it.

### 3. CMake  (only if not using VS's bundled copy)
VS 2022's C++ workload already includes CMake. For a standalone copy on PATH,
install from <https://cmake.org/download/> (Windows x64 installer, "add to
PATH"). The project needs CMake **3.20+**.

### 4. SDRplay API for Windows  (the one proprietary piece)
Download the **Windows API/HW driver** from <https://www.sdrplay.com/api/> (or
the downloads page). It installs to `C:\Program Files\SDRplay\API\` with:

- `x64\sdrplay_api.dll`  + `x64\sdrplay_api.lib`  (import lib)
- `inc\sdrplay_api.h`
- the **`sdrplay_apiService.exe`** Windows service (must be running to stream)

Same rule as Linux: **never bundle this lib in a release** — it's installed
separately by the operator.

### 5. rnnoise  (noise-reduction library — hard link dependency)
The vcpkg port does **not** build on MSVC (marked `!windows`; its
autotools/libtool shim chokes on `cl.exe` — verified 2026-08-31). Build from
source instead; the library is ten portable C files:

1. Get the rnnoise 0.2 source (xiph release tarball, or the tree vcpkg
   extracts to `buildtrees/rnnoise/src/…` before failing).
2. Drop in the ~25-line `CMakeLists.txt` used on this station
   (`C:\Users\jon55\third_party\rnnoise-0.2\CMakeLists.txt`): a STATIC lib of
   the ten `RNNOISE_SOURCES` from Makefile.am, plus
   `target_compile_definitions(rnnoise PRIVATE __SSE2__)` for MSVC — MSVC
   never predefines `__SSE2__`, and without it `vec.h` falls into a generic
   path that includes `os_support.h`, a header the release doesn't ship.
3. `cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release`
   `-DCMAKE_INSTALL_PREFIX=C:\Users\jon55\third_party\rnnoise-install`,
   build, install.
4. Add that install prefix to the console's `CMAKE_PREFIX_PATH` (below).

### 6. Git
Already installed (`git --version` ≥ 2.55). ✓

---

## Verify the toolchain

Open the **"x64 Native Tools Command Prompt for VS 2022"** (Start menu — this
sets up the MSVC environment) and check:

```bat
cl                & rem  MSVC compiler banner
cmake --version   & rem  >= 3.20
ninja --version   & rem  bundled with VS
```

---

## Build (target state — not all source ports are done yet)

From the `x64 Native Tools Command Prompt`, in the repo root:

```bat
cmake -B build-win -G Ninja ^
  -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
  -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64;C:/Users/jon55/third_party/rnnoise-install" ^
  -DBUILD_SDRPLAY=ON
cmake --build build-win -j12
```

Adjust the Qt path to your actual install. Drop `-DBUILD_SDRPLAY=ON` to build
the core app without the SDR source while bringing the port up.

**`-DCMAKE_BUILD_TYPE=RelWithDebInfo` is NOT optional here.** VS's bundled
CMake is Microsoft's fork (`x.y.z-msvcN`) and it initializes
`CMAKE_BUILD_TYPE` to **Debug** instead of leaving it empty, which silently
defeats the top-of-CMakeLists RelWithDebInfo default. A Debug build cannot
keep up with the 1 MHz IQ stream (see CLAUDE.md — this exact trap cost an
evening on Linux). Pass it explicitly on every fresh configure.

To run, Qt's DLLs must be findable — either run from the Qt `bin` on PATH, or
`windeployqt build\tentec-console.exe` to copy the runtime beside the binary.

---

## Port status (what still needs Windows work)

See the porting plan. In dependency order:

1. **Serial transport** — `src/radio/SerialPort.cpp` rewritten on `QSerialPort`
   (POSIX `termios`/`ioctl`/`QSocketNotifier` today). Fixed public interface.
2. **Audio I/O** — `AudioCwSource`, `RipAudio`, `TripAudio`, `ClipDeck` move
   from PipeWire/Pulse CLI (`parec`/`pacat`/`pw-play`/`pactl`) to Qt Multimedia
   (`QAudioSource`/`QAudioSink`/`QMediaDevices`) behind a small backend seam.
3. **Crash handler** — `src/app/main.cpp` `execinfo`/`isatty` behind `#ifdef`.
4. **CMake** — SDRplay + rnnoise + Qt Multimedia find/link for Windows.
5. **Device defaults** — `/dev/*` hints → `COMx`; enumeration already uses
   `QSerialPortInfo` and degrades cleanly.
6. **Optional Linux integrations** — cqrlog self-disables; guard the
   hardcoded `~/.config` paths. **voacapl no longer self-disables** — the
   VOACAP overlay is a shipped Windows feature (see below).
7. **Packaging** — `packaging\make-windows-release.ps1` builds the release
   zip on this box and attaches it to the tag's GitHub Release (CI cuts the
   Linux AppImage, this box cuts Windows — so every shipped exe was built
   where it gets tested). With HEAD on the release tag, from the x64 Native
   Tools prompt:

   ```bat
   powershell -ExecutionPolicy Bypass -File packaging\make-windows-release.ps1
   ```

   It refuses a dirty tree or an untagged HEAD, stages the Qt runtime with
   `windeployqt`, hard-stops if any `sdrplay_api*` file lands in the stage
   (never redistribute it), and uploads with `gh`. `-NoUpload` for a dry
   run; `-QtDir`/`-RnnoiseDir`/`-VoacapDir` if the installs move. An
   Inno/NSIS installer can come later; the zip is the alpha-tester
   deliverable.

## VOACAP engine for Windows (voacapl.exe + itshfbc)

The AppImage workflow builds voacapl inside CI; on Windows it is prebuilt
**once** on this box and the release script copies it in. Rebuild it only
when voacapl itself changes. Default location — override with `-VoacapDir`:

    C:\Users\jon55\third_party\voacapl-win\
      voacapl.exe        statically linked, so the zip needs no MinGW DLLs
      itshfbc\           45 coefficient files, no symlinks

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

`ls $DEST/itshfbc/coeffs | wc -l` must print **45**. The release script
enforces that too — the AppImage once shipped 1 of 45 because the copy
preserved makeitshfbc's symlinks back to the build prefix, and the engine
crashed. On MSYS2 `makeitshfbc` copies rather than links, but `-L` keeps
that from mattering.

Note the ITSHFBC tree that a **VOACAP/ICEPAC for Windows** installer drops
in `C:\itshfbc` is *not* interchangeable: an ICEPAC install carries only the
`*W.BIN` coefficient variants, so `itshfbcDir()` deliberately never looks
there — it uses the bundled tree, seeded into `%APPDATA%\n8mus\tentec-console\itshfbc`
on first run because voacapl writes its decks into `<root>\run`.
