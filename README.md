# Orion NCC-565 Warp 1

*(An independent project, not affiliated with or endorsed by Ten-Tec — or Starfleet. The name is this console's own: the hull numbers are the radios' real Ten-Tec model numbers — Orion = 565, Omni VII = 588, whose personality boots as **Omni NCC-588 Warp 1** — and all signals depart at exactly Warp 1.)*

A Flex/SmartSDR-style SDR console that gives the **Ten-Tec Orion (565/566)** and
**Omni VII (588)** full computer control plus a live **panadapter/waterfall** fed by
an **SDRplay RSP2** on the radio's SPARE antenna jack — while keeping WSJT-X, fldigi,
cqrlog and GridTracker working unchanged.

The flagship feature: **drag a passband edge on the panadapter and the radio's DSP
filter follows** (continuous on the Orion; stepped on the Omni VII — a hardware limit).

> Status: **early**. Real and verified against live hardware: the Orion CAT driver,
> the `rigctld` interop server, RSP2 IQ streaming (SDRplay API v3.15), and a **live
> FFT panadapter**. Still to do: the WDSP DSP/demod path and RX audio. See
> [docs/architecture.md](docs/architecture.md) for the plan,
> [docs/phase0-latency.md](docs/phase0-latency.md) and
> [docs/phase0-sdr.md](docs/phase0-sdr.md) for the measured Phase-0 results.

## Build (Linux / Arch)

Requires: `cmake`, `ninja`, a C++20 compiler, `qt6-base`. Optional: SDRplay API.

```sh
cmake -S . -B build -G Ninja
cmake --build build
./build/tentec-console
```

Enable the SDRplay RSP2 source (needs the SDRplay API service running):

```sh
cmake -S . -B build -G Ninja -DBUILD_SDRPLAY=ON
cmake --build build
```

## What works today

- **Ten-Tec Orion driver** (`src/radio/TenTecOrion.*`) — real CAT encoders for
  frequency, mode, bandwidth (`*RMF`), and PBT (`*RMP`), plus the `setPassband()`
  bandwidth+PBT decomposition. Serial via a POSIX wrapper integrated into the Qt loop.
  The app opens `/dev/orion` (override with `TTC_RADIO_DEV`) and polls VFO-A at
  ~5 Hz, so click-to-tune and drag-to-filter command the radio and the panadapter
  follows the physical dial.
- **rigctld emulation** (`src/net/RigctldServer.*`) — listens on TCP **4532** so
  cqrlog / WSJT-X / fldigi / GridTracker connect here instead of flrig. Handles
  `f/F/m/M/v/t/\chk_vfo/\dump_state` (partial — see TODOs).
- **SDRplay RSP2 streaming** (`src/sdr/SdrPlaySource.*`, `-DBUILD_SDRPLAY=ON`) —
  real IQ from the RSP2 via SDRplay API v3.15, verified live at 1.94 MS/s.
  `tools/sdr_probe` is a headless smoke test. Gain defaults tuned for the hot
  shared-antenna feed (gRdB 40 / LNAstate 6).
- **Live panadapter** (`src/dsp/*`, `src/ui/PanadapterWidget.*`) — RSP2 IQ →
  windowed FFT → power spectrum, marshaled to the GUI thread and drawn as a live
  250 kHz panadapter centered on the dial. Click-to-tune recenters; the
  passband-edge-drag overlay maps to the radio filter. Verified headless end-to-end.
- **DVR + voice keyer** (`src/audio/ClipDeck.*`, DVR group on the TX bar) —
  REC captures RX audio off the air from the station sound interface
  (SignaLink-style USB codec, auto-detected; `dvr/radioAudioMatch` setting
  overrides), PLAY replays the last take on the speakers or retransmits it
  (right-click). VK1–VK4 store voice-keyer messages: right-click records from
  the mic (`dvr/micSource`, default input if unset), click transmits the
  message with CAT PTT held. Rides `pw-record`/`pw-play`, WAVs live in
  `~/.local/share/n8mus/tentec-console/dvr/`.

## Layout

```
src/app/    Qt entry point + main window (wires the pieces together)
src/radio/  RadioController interface, Ten-Tec Orion driver, SerialPort
src/net/    rigctld-compatible TCP server (client interop seam)
src/sdr/    SdrSource interface + optional SDRplay RSP source
src/ui/     PanadapterWidget
```

## Next steps (Phase 0)

1. Wire the real Orion serial device (e.g. `/dev/ttyS0`) and **measure command
   round-trip latency** — it determines how the filter drag feels.
2. Turn on `-DBUILD_SDRPLAY=ON` and stream RSP2 IQ into the panadapter.
3. Vendor the NereusSDR/WDSP foundation for the real DSP + waterfall.

License: GPL-2.0-or-later (to reuse WDSP / the Thetis lineage). See [LICENSE](LICENSE).
