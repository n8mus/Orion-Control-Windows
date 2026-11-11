# RADE (FreeDV Radio Autoencoder) for Windows

The Linux build fetches rade_c and Opus and compiles them inside CMake
(`../CMakeLists.txt`). That recipe runs Opus's autotools build, which does
not exist under MSVC, so on Windows the same pinned sources are built once
with the two scripts here and the console links the result
(`-DRADE_WIN_ROOT=<work-dir>/install`, picked up by
`../windows-prebuilt.cmake`).

Everything is a static MSVC library (x64, `/MD`, Release), so nothing extra
ships beside the exe.

## Build

1. In **Git Bash**:
   `sh third_party/rade/windows/fetch-and-patch.sh C:/path/to/rade-win`
   Downloads rade_c, Opus and the Opus DNN model data at the pins in
   `../TTC-VENDOR.md`, checks each SHA-256, and applies the patches.
2. In the **x64 Native Tools Command Prompt for VS 2022**:
   `third_party\rade\windows\build.bat C:\path\to\rade-win`
3. Configure the console with `-DRADE_WIN_ROOT=C:/path/to/rade-win/install`.
   CMake prints `RADE: prebuilt from ...`.

Check it with the console's own offline tests (no radio, nothing keys):
`cmake --build build-win --target raderxtest radetxtest`, then run both.

## What differs from the Linux recipe

- **Opus is built with its own CMake** (`OPUS_DRED=ON OPUS_OSCE=ON`, static)
  instead of `autogen.sh && configure`. The DNN model data that `autogen.sh`
  would download is fetched by `fetch-and-patch.sh` at the same hash.
- **One local patch**, `rade_c-rade_api.h-static-export.diff`: on Windows
  `rade_api.h` puts `__declspec(...) __stdcall` in front of every return
  type, which cl.exe cannot parse. The patch lets the build define
  `RADE_EXPORT` itself, and everything here is compiled with it empty. The
  console's imported target passes the same definition to its own sources.
- **The shim's Opus configuration is passed as compile definitions.** Opus's
  CMake leaves `config.h` empty and puts its configuration on the command
  line, so `ttc_rade_shim.c` gets the same list explicitly (`CMakeLists.txt`
  here). Without it `opus_select_arch()` quietly becomes `return 0` and the
  transmit side's feature extraction never leaves the plain-C kernels. Keep
  that list equal to the opus target's `DEFINES` in
  `<work-dir>/build/opus-msvc/build.ninja`.

## CPU support

Opus picks SSE4.1 or AVX2 kernels at run time and presumes only SSE2, so the
libraries run on any x64 CPU. Measured on an i7-1355U with the off-air test
recording: about 5% of a core to receive with AVX2, about 23% forced down to
SSE2.

The libraries are Release `/MD`: right for a Release or RelWithDebInfo
console, not for a Debug (`/MDd`) one.
