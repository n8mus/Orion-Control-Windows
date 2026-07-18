# USS Orion NCC-565

*(An independent project, not affiliated with or endorsed by Ten-Tec — or
Starfleet. The hull numbers are the radios' real Ten-Tec model numbers:
Orion = 565, Orion II = 566, Omni VII = 588.)*

A Flex/SmartSDR-style console for the **Ten-Tec Orion (565/566)**: full
computer control, a live wideband **panadapter/waterfall** fed by an SDRplay
RSP, drag-the-filter-edges tuning, a draggable notch, CW decode and a
multi-channel **CW skimmer**, DX-cluster/RBN/POTA spots painted on the
spectrum, WinKeyer keying, rotor control, and `rigctld` emulation so
**WSJT-X, fldigi, cqrlog and GridTracker keep working unchanged** — one
serial port, no flrig.

> **Status: ALPHA, Orion-only.** Daily-driven at one station for months;
> you are among the first others to run it. Expect rough edges, report
> everything. The Omni VII personality exists in the code but is currently
> untested — stay on the Orion for now.

## Install (easiest — one line)

New to Linux? This script does everything for you: installs the libraries,
sets up the SDRplay API and serial-port access, downloads the latest
console, and adds it to your applications menu.

```
bash <(curl -fsSL https://raw.githubusercontent.com/n8mus/orion3-sdr-console/master/install.sh)
```

Log out and back in once after the first run (for serial access), then
launch **Ten-Tec SDR Console** from your menu. Re-run any time to update.
Prefer to build from source? See [Build and run](#build-and-run) below.

## What you need

**Hardware**
- **Ten-Tec Orion 565 or Orion II 566** (developed against V3 firmware) with
  a serial CAT connection — native RS-232 or a *quality* USB-serial adapter
  (a cheap one cost this station weeks of RFI hunting; FTDI on a direct
  motherboard port, or a real COM port, strongly advised).
- **SDRplay RSP** (developed on an RSP2; nearby models likely work) fed from
  the Orion's spare RX antenna jack or an antenna tap. ⚠️ **The SDR feed
  MUST be TX-protected.** If your tap does not isolate the SDR when you
  transmit, 100 W will destroy it. Know your isolation before you key up.
- A monitor **at least 1920 px wide** (the layout needs 1808 px).
- Optional, each unlocking a feature: **WinKeyer USB** (CW keying, memories,
  type-ahead), a **USB audio codec / SignaLink** carrying the radio's RX
  audio (CW decode through the radio's own filters + the pitch meter),
  a **rotctld-compatible rotor**, and **cqrlog** (one-click QSO logging via
  the console's UDP bridge).

**Software (Linux; developed on Arch)**
- Qt 6 (base + serialport), `cmake`, a C++20 compiler, `rnnoise`.
- The **SDRplay API v3.x service** from
  [sdrplay.com/downloads](https://www.sdrplay.com/downloads/) — proprietary,
  installed by you, never bundled here. The `sdrplay_apiService` must be
  running (their installer sets that up).
- Optional: `hamlib` (rotctld for the rotor); PipeWire/PulseAudio utilities
  (`pactl`, `pw-record` — present on any modern desktop) for the audio
  features.

## Build and run

```sh
cmake -S . -B build -DBUILD_SDRPLAY=ON
cmake --build build -j$(nproc)
./build/tentec-console
```

## First run

The **Station setup** dialog opens by itself: your callsign and grid, the
radio model and CAT serial port, WinKeyer port, radio-audio capture device,
DX-cluster login, rotor. The **Test** buttons probe the real hardware — the
radio test shows the Orion's firmware string, the keyer test runs the
WinKeyer handshake. It lives in **SDR ▾ → Station setup…** afterwards.
Radio model/port changes take effect on the next launch.

Then:
- **rigctld emulation is on TCP 4532.** Point WSJT-X / cqrlog / GridTracker
  there (any rigctld-compatible client works). Stop any real `rigctld` or
  flrig instance first — one master per radio.
- The panadapter's gain is managed by **Auto LNA** (SDR ▾ menu). A loud
  evening band stepping the attenuation up is normal operation, not a fault.
- Click to tune (CW zap snaps to the carrier), wheel to step, drag a
  passband edge to cut the filter, drag the passband body to slide PBT,
  right-click to park VFO B, `Z` zero-beats, `X` flips CW sideband.

## Reporting problems

Run from a terminal and include everything it printed. If it crashed,
`coredumpctl list` should show it — `coredumpctl info` output is gold.
Say what band, what mode, what you clicked, and whether the radio, the
SDR, or both were connected. Screenshots welcome.

## Known alpha limitations

- **US band plan** assumptions (60 m channels, CW skimmer windows,
  band-plan shading).
- Orion-only until an Omni VII CAT path is re-established and tested.
- Linux-only today; a Windows port is planned after the alpha settles.
- One console instance at a time (exclusive serial + single-tuner SDR).

## License

**GPL-3.0** for the combined work: the CW decode engine is a port of
fldigi's (© W1HKJ & contributors, GPL-3), which lifts the rest of this
GPL-2.0-or-later codebase to GPL-3 in combination. You received this
software with its complete source; if you pass binaries along, the source
goes with them. The SDRplay API is proprietary and is **not** part of this
distribution — every user installs it themselves from SDRplay.

73 — and all signals depart at exactly Warp 1.
