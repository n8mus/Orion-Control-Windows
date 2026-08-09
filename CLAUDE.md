# Orion NCC-565 Warp 1  (Omni personality: Omni NCC-588 Warp 1)

Qt6/C++20 Flex-style console for Ten-Tec Orion (565/566) and Omni VII
radios with an SDRplay RSP2 panadapter. One binary, two personalities
(`TTC_RADIO=orion|omni8`, or the `radio/model` setting). Linux only.
The hull numbers are the real Ten-Tec model numbers; the binary stays
`tentec-console` (launchers and docs depend on it).

## Build & run

    cmake -B build -DBUILD_SDRPLAY=ON && cmake --build build -j8
    ./build/tentec-console

The default build type is **RelWithDebInfo** (since 2026-08-07) — NOT
Debug. This is load-bearing: an unoptimized DSP chain cannot keep up
with the 1 MHz IQ stream and the SDRplay service silently drops
transfers to ~40%, which shreds decode timing (a whole evening of "the
skimmer is broken" traced to a Debug build). The IQ fan-out (FFT +
24-channel skimmer bank + SKIM-view STFT + recorder) runs on a
dedicated **worker thread** off the USB callback, fed by a 2-second
sample-capped ring; the callback only copies+enqueues. The console
watches its own feed — a status-bar warning fires below 95% delivery
("USB starving" vs "IQ worker … CPU overloaded"). The skimmer bank is
OpenMP-parallel (optional; `find_package(OpenMP QUIET)`, pragma is a
no-op without it). If spots/decode look like garbage, CHECK THE STATUS
BAR FIRST — starved samples mimic every decoder bug.

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
| `cwtest` | decoder quality matrix, prints % (full ~8 min, `--quick` ~90 s, `--only <substr>` isolates rows) | — |
| `skimtest` | 10-station synthetic segment through the skimmer | — |
| `skimsrvtest` | RBN telnet protocol/format/throttle | — |
| `fldigitest` | fldigi XML-RPC client | `python3 tests/fake_fldigi.py 17362 &` (or `FLTEST_PORT`/`FLTEST_LOOSE` vs real fldigi) |
| `rotortest` | rotctld client (the `rotor/mode=rotctld` fallback) | `rotctld -m 1 -t 14533 &` (dummy turns ~6°/s — be patient) |
| `dcutest` | DCU-3 frame parser (streams-while-turning captures) + the rotctld server we put in front of it | — |
| `watchtest` | DX-watch pattern matching | — |
| `lptest` | LP-100A wattmeter frame decode (pure parser, no hardware) | — |
| `pmtest` | PowerMaster wattmeter frame decode + CRC (pure parser, no hardware) | — |
| `smithtest` | renders a band's stored sweep to PNG for eyeballing (`QT_QPA_PLATFORM=offscreen ./smithtest <swr.json> <band> <out.png>`) | — |
| `nrtest` | noise-reduction ruler: keyed 550 Hz tone at swept SNR through {none, RNNoise, SpectralNr} into the audio-path decoder. Verdict 2026-07-16: RNNoise perfect to -6 dB; our SpectralNr lost and stays test-only | — |

Run the relevant tests plus a selftest sizing check before every commit.
`cwtest` full-matrix baselines (2026-08-06): legacy 94%, engine 95%,
bayes 100% — treat any drop as a regression, per brain. Decoder changes
must keep "dead channel" quiet (noise babble was a real on-air bug,
twice); the bayes brain's bar there is ≤2 garbage chars (was 0 before
the 2026-08-07 QRN spike gate — one statistical-tail char per 60 s was
the price of storm-proof copy on a live lightning night, measured on
that night's own capture).

## Architecture in 60 seconds

- **Offset LO**: the SDR tunes `kLoOffsetHz` (260 kHz) *above* the dial
  (PowerSDR if_freq idea) so the zero-IF DC hump never lands on the tuned
  signal or in the view (max dial-centered view = capture − 2·offset =
  480 kHz). Capture is `kSdrCaptureHz` (1 MHz: 2 MS/s, hw decim 2), FFT
  16384 (~61 Hz/bin). At runtime use `loOffHz_`, never the constant — CTUN
  and the band-overview frames (band buttons land framing the whole band,
  LO parked just above the frame) move the LO. Everything mapping
  bins↔frequency goes through `absHz = LO + (bin - n/2) * binHz`.
  Constants + `edgesFromRig()` passband math live in
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
  skimmer finds = DX-cluster telnet `:7300` (cqrlog AND Not1MM point
    their band map here — server `127.0.0.1` port `7300`; the greeting
    carries the word HELLO because Not1MM's client waits for it before
    reading spots, cqrlog ignores it);
  QSO logging = UDP ADIF datagram to cqrlog's bridge on `:2334`.
  New integrations should follow this pattern, not grab devices.
- **Spot pipeline**: sources (cluster telnet, POTA API, own skimmer) →
  `pushSpots` lambda in MainWindow.cpp → `PanadapterWidget::setSpots`.
  Kinds: 'D' DX yellow, 'P' POTA green, 'F' FT8 cyan, 'S' skimmer violet;
  call text is colored by cqrlog worked-before status (LogbookIndex),
  watch-list hits get an orange ring.
- **SKIM view** (SKIM ▾ → Waterfall view): CW-Skimmer-style zoomed
  waterfall — `src/cw/SkimStft` (4096-pt STFT at ~125 ksps, 31 Hz/bin,
  8 ms columns, fed in the iqHandler on the SDR thread, gated by window
  visibility) + `src/ui/SkimViewWindow` (frequency vertical, time
  flowing left, keying visible per station, skimmer calls labeled next
  to traces in worked-before colors, click tunes / wheel zooms / drag
  pans). Headless check: `TTC_SKIMVIEW=<png>` opens it at startup and
  grabs it (pair with `TTC_IQFILE` + `TTC_IQFAST=1` on a ground-truth
  capture). Old 500 ksps captures replay correctly — the pipeline
  rescales from the header rate (the LO-offset era is derived from it:
  60 kHz at 500 k, 260 kHz at 1 M).
- **CW decoding** (`src/cw/CwDecoder`): IQ-fed, one recurrence-phasor
  mixer per instance, so banks are cheap (SkimmerEngine runs 24). THREE
  decode brains behind one front end: the legacy battle-tuned slicer
  (every threshold has an on-air story in a comment),
  `src/cw/FldigiCwEngine` — fldigi's decode logic ported under
  GPL-3 (W1HKJ et al.; keep the attribution header) — used by the CW
  window's tuned reader with FLD/SOM/DEEP/ATK/DCY controls, and
  `src/cw/BayesCwEngine` (2026-08-06): two-state HMM soft keying +
  whole-character duration inference with ins/del/split/merge alignment
  over marks AND gaps, boundaries at MAP points against a LEARNED
  element gap. Full matrix: bayes 100%, engine 95%, legacy 94% (engine
  wins sloppy timing via SOM but loses a little at 45+ WPM; bayes takes
  both; quick-tier QSB rows under-read the engine — its trackers need
  the full-length runs to warm up, so the quick sentinel is
  `TTC_CWLEGACY=1 cwtest --quick` = 100%; gates: engine full matrix
  ≥95%, bayes = 100% AND a silent dead channel). On real captures the
  bayes brain reads cleaner on solid/jittery copy and goes QUIET on
  fades where the engine strings plausible-but-wrong letters — aligned
  with the skimmer's anti-phantom rules, but the tuned-reader feel is
  the operator's call. `TTC_CWENGINE=1` / `TTC_CWBAYES=1` /
  `TTC_CWLEGACY=1` force a path in tests (legacy wins ties);
  `TTC_BAYESDBG=1` dumps per-char inference state. The ENGINE is the
  default everywhere (flipped 2026-07-15 after it out-copied real
  fldigi on the same live signal — operator-verified); the SKIM ▾ menu's
  "Bayes decoder (experimental)" toggle (`skim/bayes`) flips the
  skimmer bank live for the on-air A/B that would earn a default flip.
  The repo is public and ships GPL-3 because of the fldigi port (fine),
  and released binaries must not bundle the proprietary SDRplay lib.
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

  never appear in this repo (code, comments, docs, commits) — before
  nothing; the pattern is split so this file itself stays clean). The
  tree-only grep once let it reach public history — scrubbed by a full
  rewrite 2026-07-27, so every commit hash dates from that rewrite.
  Ideas from public manuals are fine; the name is not.
  KE9NS/PowerSDR/Thetis attribution in comments is OK.
- Never key the transmitter or send CW in unattended tests. RX-only use
  of the real SDR/cluster is fine.
- The repo is PUBLIC (alpha since 2026-07-18); the operator (Jon, N8EM)
  pushes happen only after his on-air test — build, test, commit, then
  *wait*.
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
is dead. **Serial ports are two AX99100 PCIe cards (four ports) plus the
motherboard, and everything now has a udev-stable name** — `/etc/udev/
rules.d/99-ham-usb.rules`, written 2026-08-08 after a card install
renumbered `ttyS4-7` and broke every hardcoded path (rotctld spent half
an hour writing rotor commands into the Omni VII). **Always use the
names, never a raw `ttyS*`:** `/dev/orion` (mobo ttyS0), `/dev/lpmeter`,
`/dev/omni7`, `/dev/rotor`. The physical connectors are also named
`/dev/portS0` and `/dev/portS4..S7`, deliberately matching the **stickers
the operator put on the brackets** (he labelled them with that day's ttyS
numbers) — the symlinks are pinned to PCI slot addresses, so those labels
stay true even if the kernel renumbers later. PCIe ports are matched by
PCI address, USB adapters by chip serial. Note `ax99100-intx.service`
still hardcodes `0000:03:00.*` (it forces legacy INTx; only the original
card ever needed it), so re-verify after any card change. **The DCU-3
rotor is on RS-232 at `/dev/portS4`** (4800 8N1; answers `NNN;` in
~271 ms). **The console now drives it directly and re-serves the rotctld
protocol on `:4533` itself** (`rotor/mode=direct`, `rotor/device`;
`DcuRotor` + `RotorServer` + `RotorLink`, `dcutest`) — same single-master
pattern as the rig on `:4532`, so cqrlog/Not1MM point at `:4533`
unchanged. Reason: hamlib's DCU backend reads a fixed 4 bytes per query,
but the DCU-3 *streams* its heading while turning, so a backlog builds
and the readback freezes on an old frame forever while aiming still
works ("rose stuck north, DCU-3 reads correct", 21 h). Port-independent,
so USB would freeze the same way. **The `rotctld.service` + watchdog are
retired 2026-08-09** — do NOT re-enable them (they fight the console for
`/dev/portS4`); set `rotor/mode=rotctld` in Setup to fall back to a
daemon (a different rotor, remote, or the DCU-3 back on USB). It was
silent on every PCIe port for most of
2026-08-08 because the controller has an internal 3-pin jumper selecting
RS-232 *or* USB, never both (manual p11) — it sat on `U`, so the RS-232
INPUT jack was electrically dead. Nothing was ever wrong with the cards
or the cables. The jumper is now on `R`, which **disables the DCU-3's
built-in USB port**, so `/dev/rotor` no longer exists and the WinKeyer is
the last USB serial device at the station. Put the jumper back on `U` and
that name returns on its own (the udev rule is deliberately kept).
The Omni VII therefore HAS a serial CAT path again (`radio/deviceSerial`,
57600 8N1 **with RTS/CTS** — read back `?A` = 50.313 MHz live on
2026-08-08); RTS/CTS is not optional, without it replies come back
truncated or as the `Z` error. Two **external wattmeters** share one
straight-through DB9 cable (never a null modem — it reflects DTR/RTS and
reads dead) into an AX99100 PCIe card, selected by `meter/model`
(`meter/{enabled,device}`,
env `TTC_LPMETER_DEV`; old `lp100a/*` keys are read as defaults and
mirrored on save): the **TelePost LP-100A** (vector; poll `;P?` at 115200
8N1 → 43-byte `;`-led CSV of power/|Z|/phase/dBm/SWR, `src/radio/LpMeter`;
mode control is `F` NOT `M` — bench-verified, `M` walks the LCD pages
invisibly) and the **Array Solutions PowerMaster** (scalar; 38400 8N1,
9-byte activation prefixed with CR to flush its jam-prone command parser,
then a ~25 Hz `(STX)D,fwd,rev,swr,…(ETX)<crc>` stream, CRC-8 poly 0xB1
init 0 xorout FF over payload, `src/radio/PmMeter`). Both are opt-in
(default OFF) behind `src/radio/TxMeter`; sweeps record R+jX (and offer
the Smith chart) only from the LP-100A, and fall back to the radio's
`@STF` without either — a station with no meter behaves as before.
SignaLink = "USB AUDIO CODEC" in PipeWire. cqrlog
runs the fork at `/home/jon/CQRLog` (bridge :2334, embedded MariaDB).
If the panadapter comes up dark ("no IQ source") right after
an unclean console death, the sdrplay_apiService handshake wedged —
one console relaunch clears it.
