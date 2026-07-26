# Orion NCC-565 Warp 1  (Omni personality: Omni NCC-588 Warp 1)

Qt6/C++20 Flex-style console for Ten-Tec Orion (565/566) and Omni VII
radios with an SDRplay RSP2 panadapter. One binary, two personalities
(`TTC_RADIO=orion|omni8`, or the `radio/model` setting). Linux only.
The hull numbers are the real Ten-Tec model numbers; the binary stays
`tentec-console` (launchers and docs depend on it).

## Build & run

    cmake -B build -DBUILD_SDRPLAY=ON && cmake --build build -j8
    ./build/tentec-console

ccache is used automatically when installed. Headless verification (also
releases the SDR cleanly on exit):

    QT_QPA_PLATFORM=offscreen TTC_SELFTEST=<secs> TTC_RIGCTLD_PORT=14599 \
      TTC_RADIO=orion TTC_RADIO_DEV=/dev/null ./build/tentec-console

`TTC_SCREENSHOT=<png>` grabs the window before quitting; `TTC_MAXIMIZED=1`
forces full-screen geometry (headless has no WM). For layout review use
`xvfb-run -a -s "-screen 0 1920x1080x24"` — the offscreen platform's
default screen distorts grabs.

## The width budget (do not break the maximize button)

The selftest prints `[layout] window WxH layout-min WxH`. Since the CI
round (2026-07-16) TTC_SELFTEST pins the font to DejaVu Sans 10 so the
number is the same ruler on every platform — on that ruler today's code
reads **1848**, and the budget is **≤ 1850 px** (enforced locally AND by
CI's layout step; the operator's real screen is 1920 with a narrower
default font, ≈ 1808 real, so 1848-pinned fits — past the budget the
window manager loses the maximize button, happened twice). Historic
numbers before the pin (1808) were the Arch-default-font ruler; don't
compare across rulers. Measure with a CLEAN config
(`XDG_CONFIG_HOME=<tmp>`): the operator's live settings can add width
(e.g. Large VFO digits ≈ +14 px) — that's his trade-off, not a
regression. The budget is about the CODE's floor. **New toolbar buttons go on the second
deck (`topLay2_`), never the first (`topLay`).** Each tool is built by its
own `setup*Ui()` in `src/app/MainWindowTools.cpp`; add a new function
there plus one call in the constructor.

## Tests (all EXCLUDE_FROM_ALL; build the target explicitly)

For the record→replay decoder loop use an optimized build — Debug runs
slower than real time on long captures:

    cmake -B build-rel -DBUILD_SDRPLAY=ON -DCMAKE_BUILD_TYPE=Release
    cmake --build build-rel --target skimreplay cwtest -j8

Ground-truth captures live in `~/.local/share/n8mus/tentec-console/iq/`
(the operator records via SDR ▾ while working stations he can copy by
ear and tells you the calls). W1AW code practice (7047.5 / 14047.5,
machine-perfect sending) is the clean decoder benchmark; sloppy human
senders and weak-signal cases are the hard set.


| target | what | pairs with |
|---|---|---|
| `cwtest` | decoder quality matrix, prints % (full ~8 min, `--quick` ~90 s) | — |
| `skimtest` | 10-station synthetic segment through the skimmer | — |
| `skimsrvtest` | RBN telnet protocol/format/throttle | — |
| `fldigitest` | fldigi XML-RPC client | `python3 tests/fake_fldigi.py 17362 &` (or `FLTEST_PORT`/`FLTEST_LOOSE` vs real fldigi) |
| `rotortest` | rotctld client | `rotctld -m 1 -t 14533 &` (dummy turns ~6°/s — be patient) |
| `watchtest` | DX-watch pattern matching | — |
| `nrtest` | noise-reduction ruler: keyed 550 Hz tone at swept SNR through {none, RNNoise, SpectralNr} into the audio-path decoder. Verdict 2026-07-16: RNNoise perfect to -6 dB; our SpectralNr lost and stays test-only | — |

Run the relevant tests plus a selftest sizing check before every commit.
`cwtest` full matrix currently scores 94% — treat any drop as a
regression. Decoder changes must keep "dead channel" quiet (noise babble
was a real on-air bug, twice).

## Architecture in 60 seconds

- **Offset LO**: the SDR always tunes `kLoOffsetHz` (60 kHz) *above* the
  dial (PowerSDR if_freq idea) so the zero-IF DC hump never lands on the
  tuned signal. Everything mapping bins↔frequency goes through this:
  `absHz = dial + kLoOffsetHz + (bin - n/2) * binHz`, span 500 kHz,
  n=8192 (~61 Hz/bin). Constants + `edgesFromRig()` passband math live in
  `src/app/MainWindowInternal.h`.
- **MainWindow** is several translation units, same class:
  `MainWindow.cpp` (constructor: layout, menus, radio polling wiring),
  `MainWindowTools.cpp` (second-deck tools), `MainWindowTuning.cpp`
  (tune/zap/filters/notch), `MainWindowOps.cpp` (band stack, TX profiles,
  DVR, 60 m channels).
- **Interop pattern — the console is the single master of each shared
  resource**, everything else connects to a service it exposes:
  rig = rigctld server `:4532` (fldigi/cqrlog/wsjt-x point here);
  WinKeyer = cwdaemon server `:6789` (cqrlog keys through us);
  skimmer finds = DX-cluster telnet `:7300`;
  QSO logging = UDP ADIF datagram to cqrlog's bridge on `:2334`.
  New integrations should follow this pattern, not grab devices.
- **Spot pipeline**: sources (cluster telnet, POTA API, own skimmer) →
  `pushSpots` lambda in MainWindow.cpp → `PanadapterWidget::setSpots`.
  Kinds: 'D' DX yellow, 'P' POTA green, 'F' FT8 cyan, 'S' skimmer violet;
  call text is colored by cqrlog worked-before status (LogbookIndex),
  watch-list hits get an orange ring.
- **CW decoding** (`src/cw/CwDecoder`): IQ-fed, one recurrence-phasor
  mixer per instance, so banks are cheap (SkimmerEngine runs 24). TWO
  decode brains behind one front end: the legacy battle-tuned slicer
  (skimmer default; every threshold has an on-air story in a comment)
  and `src/cw/FldigiCwEngine` — fldigi's decode logic ported under
  GPL-3 (W1HKJ et al.; keep the attribution header) — used by the CW
  window's tuned reader with FLD/SOM/DEEP/ATK/DCY controls. Full matrix:
  engine 95%, legacy 94% (engine wins sloppy timing via SOM, loses a
  little at 45+ WPM; quick-tier QSB rows under-read the engine — its
  trackers need the full-length runs to warm up, so the quick sentinel
  is `TTC_CWLEGACY=1 cwtest --quick` = 100%; the engine gate is the
  full matrix ≥95%). `TTC_CWENGINE=1` / `TTC_CWLEGACY=1` force either
  path in tests. The ENGINE is the default everywhere (flipped
  2026-07-15 after it out-copied real fldigi on the same live signal —
  operator-verified). If the
  repo ever goes public it ships GPL-3 because of this file (fine), and
  binaries must not bundle the proprietary SDRplay lib.
  Tried and rejected (2026-07-14): an fldigi-style per-element release
  threshold (`off = 0.35 × this element's own peak`). The fast in-key
  peak re-training already collapses the tracker to the current
  element within ~10 ms, so the guarded variant replayed byte-identical
  on every ground-truth capture, and unguarded it merged shallow
  inter-element gaps on clean W1AW (DISPLAYED→DISPLAYEB). Weak-station
  copy is gated by the antenna feed SNR, not element release.
- **Radios**: both are carrier-at-dial in CW (no pitch math anywhere).
  Orion = ASCII CAT, dual RX; Omni VII = binary CAT ("omni8"
  personality). Quirks are documented where they're handled — grep
  `live-verified` / `live-found`.

## Hard rules

  never appear in this repo (code, comments, docs, commits) — run
  pattern is split so this file itself stays clean). Ideas from public
  manuals are fine; the name is not. KE9NS/PowerSDR/Thetis attribution
  in comments is OK.
- Never key the transmitter or send CW in unattended tests. RX-only use
  of the real SDR/cluster is fine.
- The repo is private; the operator (Jon, N8EM) pushes happen only after
  his on-air test — build, test, commit, then *wait*.
- Settings are QSettings (`n8mus/tentec-console`) — running the binary
  uses the real config; for tests that must write settings, redirect
  with `XDG_CONFIG_HOME`.
- Never renumber stored enums (meter styles etc.) — append only.

## Station facts the code assumes

Operator callsign N8EM, grid EN83al. WinKeyer USB on the FTDI A904QF5Z
by-id path. **Orion CAT is the motherboard's native RS-232 `/dev/ttyS0`**
(`radio/device` setting; device precedence: `TTC_RADIO_DEV` env >
setting > `/dev/orion` default). The FT4232H quad converter that used
to carry both radios was condemned 2026-07-16 — it WAS the 40 m spike
picket (RFI), proven by pull-test at LNA 1; its `/dev/orion` udev rule
is dead, and the Omni VII (needs RTS/CTS) has no CAT path until a clean
converter arrives. SignaLink = "USB AUDIO CODEC" in PipeWire. cqrlog
runs the fork at `/home/jon/CQRLog` (bridge :2334, embedded MariaDB).
If the panadapter comes up dark ("no IQ source") right after
an unclean console death, the sdrplay_apiService handshake wedged —
one console relaunch clears it.
