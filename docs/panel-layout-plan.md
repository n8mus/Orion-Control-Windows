# Panel layout (second look) — execution plan

Goal: an opt-in **Panel** layout (AetherSDR/SmartSDR style: panadapter(s) in
the middle, wide right sidebar of detachable boxes for VFO A, VFO B and the
SDR as a third VFO, routing, transmit, tools; CW decoder docked under the
waterfall) **alongside** today's layout, which stays the default as
**Classic**. Mockup: claude.ai artifact "Console Panel layout mockup"
(2026-10-16).

## Rules that protect the current program

1. All work on a branch (`panel-layout`); master only moves after the
   operator's on-air test of each phase.
2. Classic is the default and never changes look or behaviour. Panel is
   chosen in Setup → Layout and applies on restart.
3. Panel stores its own settings under new keys (`ui/layout`, `panel/*`);
   it never rewrites a Classic key.
4. Every step is one small commit that can be reverted alone.
5. Before every commit: the safety-net checks below, the existing test
   suite, the selftest width check (Classic ≤ 1850), and the CI-config build
   (`BUILD_SDRPLAY=OFF`, `BUILD_GPU_PAN=OFF`).
6. Tests never key a real transmitter: CAT checks run against a fake radio
   on a pty.

## Phase 0 — safety net (no product change)

- 0.1 Branch `panel-layout` from master.
- 0.2 Golden screenshots: a script that grabs Classic headless (Xvfb,
  pinned font, clean config, replayed IQ) for a matrix — Orion and Omni VII,
  GPU on/off, 3D, contest deck open, several meter faces, FreeDV mode. The
  master baseline is stored; every later step must match it pixel for pixel
  (within anti-aliasing noise).
- 0.3 CAT transcript check: a fake radio on a pty records every command;
  a scripted UI run (band, mode, filter, AGC, routing/split, A>B, A⇄B,
  antennas, RIT…) through a test hook. The master transcript is the
  baseline; every refactor step must produce the identical transcript, for
  both personalities.

## Phase 1 — rebuild the main window's assembly (Classic identical)

- 1.1 Turn the inline controls into widgets: VFO block (frequency display,
  volume, MUTE, LOCK, CTUN, VIEW), zoom control, SDR gain panel. Classic
  places them exactly where they are today.
- 1.2 Split the 3,900-line constructor into: build widgets → wire signals →
  place (Classic).
- 1.3 A `LayoutHost` interface with `ClassicLayout` as its only
  implementation; `ui/layout` read at start-up.
- Gate: operator runs it on air; because nothing visible changed it can be
  merged to master early, keeping the branch short.

## Phase 2 — dockable box system

- 2.1 Port AetherSDR's `containers/` package (GPL-3, ~1,700 lines) to
  `src/ui/containers`: title bar with float/close, floating windows that
  remember position and monitor, re-dock, saved arrangement. Offscreen unit
  test for float/dock/save/restore.
- 2.2 Tool windows remember their geometry — Panel layout only (Classic
  keeps today's behaviour unless the operator asks).

## Phase 3 — Panel layout, first version (opt-in)

- 3.1 Right sidebar: toggle strip; boxes wrapping the existing widgets —
  Meter, VFO A, Routing (today's grid, unchanged), VFO B, Transmit (with the
  manual Tune split toggle: moves TX A⇄B only, never picks a frequency or
  keys), Tools tiles. One panadapter with the left popup column reusing the
  existing DISPLAY/SPOTS/AUDIO/SDR/SKIM/ROT menus and a BAND flyout. Slim
  top bar, status bar.
- 3.2 Setup → Layout selector; its own width check in the selftest and CI.
- 3.3 Omni VII: same gating as Classic (split-only routing, *C1V antenna
  relay, greyed controls) verified in Panel.
- Done 2026-10-16 on the branch: layout, Setup selector, CI width step
  (Panel 1597 px), Omni = same gating code. Still to do in phase 3: the
  AetherSDR analog arc meter face + shared/per-VFO meter choice, and 2.2
  (tool windows remember geometry, Panel only).
- Gate: on-air test.

## Phase 4 — CW strip and tools

- 4.1 Split the CW decoder text out of `CwWindow` so it can live in the
  docked strip or the window; decoding no longer gated on the window being
  visible; receiver selector (A first; B/SDR with phase 5).
- 4.2 Contest deck: shares the under-pan slot with the CW strip.
- Gate: on-air test.

## Phase 5 — SDR as a third VFO

- 5.1 SDR receiver with its own frequency inside the capture ("Follow A" =
  today's behaviour and the default, so Classic is unaffected).
- 5.2 A second pan for the SDR VFO (second PanadapterWidget on the same
  FFT; second GPU view).
- 5.3 Click/modifier rules for which VFO a pan click tunes; spot clicks;
  arrow keys.
- Gate: on-air test.

## Phase 6 — polish (as wanted)

Shared colour tokens instead of hand-written style strings; slice flags on
the pan; saved named layouts (everyday / contest); second-monitor placement;
later a wider capture with per-pan cut-outs, an RSPduo for two-band SO2R
pans, a general-coverage RX antenna.

## Release

Panel ships as an option in a minor version (e.g. v0.2.0) with Classic the
default; release notes say how to switch and how to switch back.

## Operator's decisions (2026-10-16)

- SDR pan: shown only when the SDR has its own frequency, and usually off —
  one obvious control to enable/disable it.
- Pan heights: selectable (equal, or one maximised).
- Spot labels: stack in rows like SmartSDR when the band is busy.
- TX bar: stays along the bottom for now, but rebuilt as its own widgets in
  phase 1 so moving it into the sidebar later is a quick change.
- Meter: selectable — one shared meter or one per VFO; add AetherSDR's big
  analog arc S-meter as a new face (appended meter style, so Classic can
  use it too without changing its default).
- Sidebar on the right; layout switch applies on restart (first version).
