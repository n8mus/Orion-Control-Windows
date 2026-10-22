# Architecture & development plan

A Flex/SmartSDR-style SDR console giving the **Ten-Tec Orion (565/566)** and
**Omni VII (588)** full computer control plus a panadapter/waterfall fed by an
**SDRplay RSP2** on the radio's SPARE antenna jack (receiver antenna-sharing tap,
TX-isolated). Must not break WSJT-X, fldigi, cqrlog, or GridTracker.

## Stack (locked decisions)

- **GUI:** C++20 / Qt6 (portable-by-construction: Linux now, macOS/Windows later
  at ~10-20% extra each, not a rewrite).
- **DSP:** WDSP (portable C, GPLv2) — the real PowerSDR/Thetis filter/demod engine.
- **Foundation:** fork of NereusSDR (Qt6/WDSP cross-platform Thetis lineage) for the
  panadapter/waterfall. This skeleton stands alone until that fork is vendored.
- **SDR input:** SDRplay API 3.15 (`libsdrplay_api`) for the RSP2; SoapySDR as a
  fallback abstraction.
- **Radio CAT:** native Ten-Tec serial driver, exposed to other apps as a
  **Hamlib `rigctld`-compatible TCP server on 4532** — so cqrlog / WSJT-X / fldigi /
  GridTracker connect unchanged. This replaces flrig (which tracks poorly).

## The flagship feature — drag the panadapter passband → radio filter

Both radios model the filter as **bandwidth + PBT** (see cat-command-reference.md).
`RadioController::setPassband(loHz, hiHz)` decomposes to bandwidth + PBT per a
per-radio `CapabilityProfile`. Orion = continuous 1 Hz (smooth); Omni VII = 37
presets (stepped).

## Phased plan

- **Phase 0 — De-risk spike:** build foundation on Arch; RSP2 → live spectrum from
  the spare jack; open Orion serial and **measure command latency** (decides drag feel).
- **Phase 1 — Driver + rigctld emulation:** capability-profiled Ten-Tec driver +
  `rigctld`-protocol TCP:4532. Ship "a better flrig" before any SDR UI.
- **Phase 2 — Panadapter:** RSP2 IQ into the console, frequency-locked to the Orion;
  PTT blanking of the SDR front-end.
- **Phase 3 — Flagship:** click-to-tune + drag-to-filter.
- **Phase 4 — Interop hardening:** WSJT-X/fldigi (SignaLink) + cqrlog + GridTracker +
  WinKeyer CW, all against :4532 at once.
- **Phase 5 — full control-suite parity:** bandmap, cluster overlay, 2nd RX, CW Skimmer feed, rotor.
- **Later — macOS / Windows packaging.**

## Directory layout

    src/app/    Qt entry point + main window
    src/radio/  RadioController interface, Ten-Tec serial driver, SerialPort
    src/net/    rigctld-compatible TCP server (client interop seam)
    src/sdr/    SdrSource interface + SDRplay RSP source (optional build)
    src/ui/     PanadapterWidget (spectrum/waterfall display)
    src/dsp/    (WDSP integration lands here in Phase 2)
