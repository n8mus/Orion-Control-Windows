# Orion Control for Windows

*(An independent project, not affiliated with or endorsed by Ten-Tec. Ten-Tec model numbers:
Orion = 565, Orion II = 566, Omni VII = 588.)*

> **This is the standalone Windows edition.** It has its own repository, its
> own releases and its own version numbers, and it is developed independently
> of the Linux console. Nothing here tracks or depends on that project.

A Flex/SmartSDR-style console for the **Ten-Tec Orion (565/566)** on Windows
10/11: full computer control, a live wideband **panadapter/waterfall** fed by
an SDRplay RSP, drag-the-filter-edges tuning, a draggable notch, CW decode
and a multi-channel **CW skimmer**, DX-cluster/RBN/POTA spots painted on the
spectrum, whole-band overview landings on the band buttons, an **SWR sweep**
plotted right on the pan, a **real-VOACAP propagation overlay**, a built-in
logbook with online log uploads, a contest deck, an RTTY modem, WinKeyer
keying, rotor control, and `rigctld` emulation so **WSJT-X, fldigi, N1MM and
GridTracker keep working unchanged**: one serial port, no flrig.

> **Status: ALPHA, Orion-only.** Daily-driven at one station; you are among
> the first others to run it. Expect rough edges, report everything. The
> Omni VII personality exists in the code but is currently untested.

## Downloads

Two builds are published on the
[Releases page](https://github.com/n8mus/Orion-Control-Windows/releases).
**Neither is "the default".** Each is listed with the date it was built;
pick the one you want. Older builds are never removed when a newer one
appears.

| Build | Version | Built | What it is |
|---|---|---|---|
| **September 2026** | `v0.1.8-alpha` | 2026-09-04 | The first complete Windows console. Smaller, simpler, and the longest-flown build on Windows. |
| **November 2026** | `v0.2.0-2026.11.11` | 2026-11-11 | Everything added since: SDR receive audio (WDSP noise blankers and noise reduction), FreeDV RADE digital voice, Copy Assist (speech to text), the contest deck with practice mode, the RTTY modem, the CW deck, logbook editing, the tuner box, the GPU panadapter, keyboard PTT. |

From `v0.2.0` on, the version number carries the build date
(`v<version>-<yyyy>.<mm>.<dd>`).

Both builds use the same settings and the same logbook, so you can go back
and forth between them on one PC.

## Install

1. **Install the SDRplay API first.** Download the Windows API/hardware
   driver from [sdrplay.com/api](https://www.sdrplay.com/api/) and run its
   installer. It is proprietary, so it is never bundled here; the console
   finds your install by itself. Skip this step and the console runs
   radio-only with a dark panadapter.
2. Download the `tentec-console-...-win64.zip` you want from
   [Releases](https://github.com/n8mus/Orion-Control-Windows/releases).
3. Unzip **the whole folder** anywhere and run `tentec-console.exe` inside
   it. The files beside the exe are needed. If Windows reports a missing
   runtime DLL, run `vc_redist.x64.exe` from the same folder once.
4. The **Station setup** dialog opens on first run. Settings live per-user
   in the registry (`HKCU\Software\n8mus\tentec-console`).

## What you need

**Hardware**
- **Ten-Tec Orion 565 or Orion II 566** (developed against V3 firmware) with
  a serial CAT connection: native RS-232 or a *quality* USB-serial adapter
  (FTDI advised).
- **SDRplay RSP** (developed on an RSP2; nearby models likely work) fed from
  the Orion's spare RX antenna jack or an antenna tap. **The SDR feed MUST
  be TX-protected.** If your tap does not isolate the SDR when you transmit,
  100 W will destroy it. Know your isolation before you key up.
- A monitor **at least 1920 px wide**.
- Optional, each unlocking a feature: **WinKeyer USB** (CW keying, memories,
  type-ahead), a **USB audio codec / SignaLink** carrying the radio's RX
  audio (CW decode through the radio's own filters, RTTY, the voice keyer,
  FreeDV), a **Hy-Gain DCU-3** rotor or any `rotctld` rotor, and a
  **TelePost LP-100A** or **Array Solutions PowerMaster** wattmeter.

**Software**
- Windows 10 or 11, 64-bit.
- The **SDRplay API v3.x** (above). Its `sdrplay_apiService` must be running;
  the installer sets that up.
- Everything else is in the zip: Qt, the VOACAP engine and, in the November
  build, FFTW (receive audio) and whisper.cpp with its English speech model
  (Copy Assist).

## Build from source

[`docs/windows-build.md`](docs/windows-build.md) is the from-scratch recipe
for a Windows build box: Visual Studio 2022 Build Tools, Qt 6.8, and the
libraries the optional features need. Release zips are cut on the station's
own Windows PC with `packaging/make-windows-release.ps1`, never by CI.

## First run

The **Station setup** dialog opens by itself: your callsign and grid, the
radio model and its COM port, WinKeyer port, radio-audio devices, DX-cluster
login, rotor, wattmeter. The **Test** buttons probe the real hardware: the
radio test shows the Orion's firmware string, the keyer test runs the
WinKeyer handshake. It lives in **SDR ▾ → Station setup…** afterwards. Radio
model/port changes take effect on the next launch. Nothing is preconfigured:
an empty field means that feature is off.

Then:
- **rigctld emulation is on TCP 4532.** Point WSJT-X / N1MM / GridTracker
  there (any rigctld-compatible client works). One master per radio: close
  any other program holding the radio's COM port first.
- **The console's own spot feed is a DX-cluster telnet server on port 7300**
  (`127.0.0.1:7300`) for a logger's band map.
- The panadapter's gain is managed by **Auto LNA** (SDR ▾ menu). A loud
  evening band stepping the attenuation up is normal operation, not a fault.
- Click to tune (CW zap snaps to the carrier), wheel to step, drag a
  passband edge to cut the filter, drag the passband body to slide PBT,
  right-click to park VFO B, `Z` zero-beats, `X` flips CW sideband.

## Reporting problems

When the console is started from its icon it writes a session log to
`%LOCALAPPDATA%\n8mus\tentec-console\console.log` (the previous session is
kept as `console.log.1`). Attach it. Say which build (the version and build
date), what band, what mode, what you clicked, and whether the radio, the
SDR, or both were connected. Screenshots welcome.

## Known alpha limitations

- **US band plan** assumptions (60 m channels, CW skimmer windows,
  band-plan shading).
- Orion-only until the Omni VII path is tested again.
- One console instance at a time (exclusive serial port, single-tuner SDR).
- Features that talk to Linux-only programs (cqrlog, Not1MM) are present in
  the menus but have nothing to talk to on Windows.
- **Winlink through Pat + ARDOP** needs `ardopcf.exe` installed separately
  and has not been flown on Windows yet.
- Copy Assist wants a CPU with AVX2 to keep up in real time.

## License

**GPL-3.0** for the combined work: the CW decode engine is a port of
fldigi's (© W1HKJ & contributors, GPL-3), which lifts the rest of this
GPL-2.0-or-later codebase to GPL-3 in combination. You received this
software with its complete source; if you pass binaries along, the source
goes with them. Which files sit under which license is in
[`NOTICE`](NOTICE), and the licenses of everything bundled in the zip are in
its `licenses` folder. The SDRplay API is proprietary and is **not** part of
this distribution: every user installs it themselves from SDRplay.

73, and all signals depart at exactly Warp 1.
