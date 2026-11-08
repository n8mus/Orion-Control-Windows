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

`TTC_SCREENSHOT=<png>` grabs the window before quitting; `TTC_CONTEST=1` opens the contest deck (`TTC_DECKTYPE=<partial>` types into CALL, `TTC_DECKPRAC=1` presses PRAC — the window-growth checks); `TTC_MAXIMIZED=1`
forces full-screen geometry (headless has no WM). For layout review use
`xvfb-run -a -s "-screen 0 1920x1080x24"` — the offscreen platform's
default screen distorts grabs.

## The width budget (do not break the maximize button)

The selftest prints `[layout] window WxH layout-min WxH`. Since the CI
round (2026-07-16) TTC_SELFTEST pins the font to DejaVu Sans 10 so the
number is the same ruler on every platform — on that ruler today's code
reads **1630** (Original view; Panel 1597), and the budget is **≤ 1850 px**
(enforced locally AND by CI's layout step; the operator's real screen is
1920 with a narrower default font — past the budget the window manager
loses the maximize button, happened twice). Until the sidebar move
(2026-10-18) the Original view read 1848 with ~2 px of slack: its menu
corner sat in the top deck. Now the floor is the TX bar (1597, full width)
or top deck (~1330) + the 300 px sidebar. Historic
numbers before the pin (1808) were the Arch-default-font ruler; don't
compare across rulers. Measure with a CLEAN config
(`XDG_CONFIG_HOME=<tmp>`): the operator's live settings can add width
(e.g. Large VFO digits ≈ +14 px) — that's his trade-off, not a
regression. The budget is about the CODE's floor. **New tool buttons go
in `topLay2_` (= `layout_->toolRow()`)**: both views re-grid it into the
sidebar (Original: the 4-across button grid; Panel: the TOOLS box), so a
button adds a row, never width. Each tool is built by its own `setup*Ui()`
in `src/app/MainWindowTools.cpp`; add a new function there plus one call
in the constructor. The grid lists it by its text (minus "▾") in DISPLAY ▾
→ Sidebar buttons.

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
| `not1mmtest` | Not1MM contest dupe sheet (`Not1mmLog`) vs a synthetic not1mm.json + ham.db + plugin: dupe_type 1/2/3/4, other contests ignored, Band fallback, inert while Not1MM isn't running; `--real` reads this station's Not1MM read-only | — |
| `rigctldtest` | the :4532 rigctld server over TCP, no radio attached (PTT only raises the signal): Pat's (Winlink) exact sequence — `dump_caps` ping, `\chk_vfo`, `\get_freq`, `\set_freq`, `t`, `\set_ptt 1/0`, ONE line back each, stream stays aligned — plus the `+` extended form unchanged | — |
| `psktest` | FreeDV reporting, offline: the PSK Reporter IPFIX datagram byte-checked against freedv-gui's layout (templates, 5-byte freq, padding, lengths) and FreeDV Reporter's reporting role (connect payload; freq/tx/rx events only after connection_successful) | — |
| `wsjtxtest` | WSJT-X UDP listener offline (FT8 button + band map, 2026-10-22): decode classification (CQ / calling me / others' QSOs), Decode -> `heard()` at dial + DF, Status -> transmitting, Heartbeat/Close -> alive, band change clears, our Close back to WSJT-X's address. WSJT-X sends multicast 224.0.0.1:2237 on `lo`; GridTracker2 listens there too, its uploads OFF (the console uploads) — docs/wsjtx-plan.md | — |
| `wltest` | Winlink session watching (`PatMonitor`): Pat's websocket JSON by hand — dialing/connected = session, gateway named, progress readout, disconnect ends it | — |
| `freedvtest` | FreeDV Reporter Socket.IO frames offline (view-only connect, ping/pong, the connect snapshot as the station table: ON AIR / TX n min ago / hours / listening / RX only, band-map vs table-only, rx_report heard spots, clamps); `--live N` watches qso.freedv.org for N s | — |
| `fldigitest` | fldigi XML-RPC client | `python3 tests/fake_fldigi.py 17362 &` (or `FLTEST_PORT`/`FLTEST_LOOSE` vs real fldigi) |
| `rotortest` | rotctld client (the `rotor/mode=rotctld` fallback) | `rotctld -m 1 -t 14533 &` (dummy turns ~6°/s — be patient) |
| `dcutest` | DCU-3 frame parser (streams-while-turning captures) + the rotctld server we put in front of it | — |
| `watchtest` | DX-watch pattern matching | — |
| `vkeqtest` | voice keyer audio chain (AetherSDR-ported gate → EQ → de-esser → compressor → final limiter, src/dsp): EQ vs its analytic curve, `snapToTargets()`, fitted presets on their targets, gate/comp/limiter/de-esser behaviour, look-ahead leveling, profiles + settings JSON (old curves load with dynamics off) | — |
| `sdrrxtest` | SDR receive chain end to end on synthetic IQ (WDSP via the decimator + resampler, audio tapped, no device): USB tone, LSB rejection, CW carrier at the pitch, AM, RX EQ peak/bypass, limiter/MUTE/BOOST/compressor/gate, NR2 on noise, and (with RADE) rade_c's off-air recording on the dial through WDSP USB -> RADE sync + speech (~70 s). Needs WDSP (FFTW3) | — |
| `asrtest` | Copy Assist: callsign extraction (one word / letter runs / hyphens / phonetics / trailing number words / non-calls), and with `~/.local/share/n8mus/tentec-console/models/ggml-base.en.bin`: G8SEZ's recording through the 48 kHz feed + VAD + whisper as ONE utterance, faster than real time, and the band-map hint turning "G-I-D-S-E-Z" into G8SEZ. Needs `pkg-config whisper` (Arch `whisper-cpp`); `TTC_ASRDBG=1` logs segments | — |
| `radetxtest` | FreeDV RADE transmit (`RadeTx`) OFFLINE loopback into RadeRx: G8SEZ's speech -> modem -> sync 97%, speech back, end-of-over decodes N8EM, 98.6% of power in 300-3000 Hz, peaks -4 dBFS, 200 ms silent tail. Never near the radio | — |
| `raderxtest` | FreeDV RADE receive (`RadeRx`) on rade_c's own 5½-min off-air recording: sync (97%), speech, end-of-over callsigns (4, last VK5KVA), noise never syncs; ~6% of a core | — |
| `containertest` | Panel-layout boxes (`src/ui/containers`, ported from AetherSDR, GPL-3): float/dock back into the same layout slot + stretch, × hides, pin = always on top, geometry/pin/state restored by a fresh manager; offscreen, temp config | — |
| `tunetest` | tuner/amp settings table behind the amber TUNER box (238C + Titan III, per antenna/band, `tuning.json`): nearest-point lookup, same-kHz overwrite, knob clamping, file round trip | — |
| `lptest` | LP-100A wattmeter frame decode (pure parser, no hardware) | — |
| `pmtest` | PowerMaster wattmeter frame decode + CRC (pure parser, no hardware) | — |
| `smithtest` | renders a stored antenna+band sweep to PNG for eyeballing (`QT_QPA_PLATFORM=offscreen ./smithtest <swr.json> <band> <out.png> [ant]`, ant defaults to unlabeled; also loads a pre-antenna file) | — |
| `cwmacrotest` | plain-language CW macros (`src/cw/CwMacros`): the `[his call]` fields, the old `%mc`/`%c` spellings, a typo left visible, the missing-field report the CW window refuses to key on, and `QsoContext`'s no-echo rule | — |
| `wktest` | WinKeyer wire format against a pty it drives itself — every timing command asserted against the K1EL WK3 manual's own worked examples, plus range clamping. Never touches `/dev/cwkeyer` | — |
| `dsstest` | AetherSDR's own DssRenderer test (`tests/dsstest.cpp` = their dss_renderer_test): frequency frames, reprojection, overhang coverage, history | — |
| `rttytest` | the RTTY core (`src/rtty`): ITA2 round trip, modulator -> demodulator loopback at 45.45/50/75 Bd and swept SNR (AWGN in 3 kHz: 100% to -3 dB, ~95% at -6), reversed tones, the IQ feed (mark +85 Hz about the channel), the AFC pulling in +80 / -180 Hz (and a no-AFC control that must lose), mismatched sense reads garbage, 30 s dead channel prints nothing, and the off-air IK3ORD capture when present (`--quick` skips the sub-zero rows) | — |
| `nrtest` | noise-reduction ruler: keyed 550 Hz tone at swept SNR through {none, RNNoise, SpectralNr} into the audio-path decoder. Verdict 2026-07-16: RNNoise perfect to -6 dB; our SpectralNr lost and stays test-only | — |

**Classic layout guard** (`tests/classic_guard.sh`, the Panel-layout work's
safety net — docs/panel-layout-plan.md): `run <binary> <dir>` drives the
console headless through `tests/uiscript/classic.txt` (TTC_UISCRIPT clicks,
TTC_NOSDR, a pty fake radio recording every SET command — never a real rig)
for six cases (Orion GPU/CPU, Omni VII, contest deck, two meter faces);
`compare <baseline> <new>` must say PASS: identical radio commands, identical
script log, screenshots within 20 px (clocks masked by `noise`). The master
baseline lives in `~/.local/share/n8mus/tentec-console/classic-guard/base`.
Safe beside the running console. Run it after every Panel-layout step.
The script addresses sliders by findChildren() ORDER (VFO volumes, then
the TX bar's) and logs a clicked button's QGroupBox title — that is why
ClassicLayout builds its sidebar only after the TX bar joined the window,
and why the lifted BAND/MODE groups keep their (transparent) titles.

**Original view sidebar** (2026-10-18, docs/original-sidebar-plan.md):
`app/SideBar` is the sidebar both views share (Panel's AppletPanel
machinery: one fixed 300 px width, a button grid, a scrolling column of
`ui/containers` boxes, drag-to-rearrange, saved order). ClassicLayout:
"▲ BUTTONS" folds the grid (`classic/side/buttonsOpen`); the grid holds
DISPLAY/SPOTS/AUDIO/SDR + the tools (moved there in `finish()`, old order)
then the box toggles; boxes ROSE, TUNER, SOLAR, METER, BAND, MODE, RECEIVE
(`classic/side/*`, separate from Panel's `panel/*`); the clock pinned at
the sidebar foot; the TX bar along the whole window bottom. The top deck
lost its menu corner and the "routing dead-center over the pan" rule.
`[classic] sidebar …` / `TTC_PANELCHECK=1` prove the width fixed, as
Panel's. DISPLAY ▾ → Sidebar buttons hides buttons (`sidebar/hidden`,
both views; a box's toggle takes its box; DISPLAY is never listed). All sidebar
buttons wear `kSideBtnStyle` (light-blue letters, white on blue when lit):
LOG / CW / EQ are window toggles (`WindowLight` keeps them lit while the
window is up; a second press or the window's × closes it), SPOTS / SDR /
SKIM / ROT light while their feature is on (Show spots / WDSP receive
audio / skimmer / rotor link — `lightWhile`, 1 s tick + menu close),
DISPLAY while its menu is open. AUDIO is a box (`ui/AudioPanel`, routing
matrix + MON) after RECEIVE, no longer a dropdown; EQ = the voice keyer
EQ & dynamics window. SOLAR has NO grid button (2026-10-19): DISPLAY ▾ →
"Solar data (NOAA)" is its switch — it already ran the NOAA poller, now it
shows/hides the box too (applied after `finish()`, or the saved box state
would win). The freed cell is METER: a `ui/MeterHost` with
`console = false` holding AetherSDR's ARC / NEEDLE / LP-100A at the box's
size, floatable like any other pane; face in `classic/side/meterFace`.
The top deck keeps OUR meter plain, as it always was — all eight faces,
SmartMTR included, click to cycle. The top-deck MeterHost of 2026-10-17
(face bar, fixed 266 px by the arc's height) was reverted 2026-10-20: it
cost 63 px of layout-min height and the operator wants that back for the
pan. AetherSDR's faces are the sidebar box's job now.

**Panel layout** (opt-in, Setup → Window → Layout, `ui/layout=panel`,
`TTC_LAYOUT=panel` in tests; applies on restart): `app/PanelLayout` answers
the same `LayoutHost` calls as Classic — popup column left of the pan (BAND
flyout = ControlPanel's band group in a popup, then the menu buttons),
sidebar of `ui/containers` boxes (meter, VFO A, routing, VFO B, transmit,
tools) beside the receive controls, TX bar along the window bottom (under
the pan alone it pushed the window to 1969 px). Boxes hide (×, toggle
strip), float (⧉, own window, geometry + pin remembered), dock back; state
in `panel/*`. Panel-only widgets (the Tune split toggle: TX A⇄B via
`*KV`, never a frequency — the routing grid's TX B click still parks B)
are built only when `layout_->isPanel()`, so Classic never gains a stray
child. Rose / tuner card / solar boxes close Panel's column. Quit freezes the arrangement on the app's Quit event (Qt 6 closes
windows after it in no fixed order — a close reads as "dock").
`tests/uiscript/panel.txt` drives it. CI checks its width too (1597 today).
Panel's meter box (`ui/MeterHost`): a face bar over our meter (CONSOLE)
and AetherSDR's own arc S-meter, cross-needle and LP-100A panel
(`src/ui/aether`, their files unchanged + small core/ stand-ins, README
there; Qt >= 6.7 only). Their faces listen to sMeterReported (dBm =
rawToDbS9 - 73) / txMeterReported and to the TxMeter (fresh < 600 ms beats
the radio); `panel/meterFace`. A static library's .qrc is never registered
on its own — their JSON lives in the app target.

Run the relevant tests plus a selftest sizing check before every commit.
`cwtest` full-matrix baselines (2026-08-06): legacy 94%, engine 95% —
treat any drop as a regression, per brain. Decoder changes must keep
"dead channel" quiet (noise babble was a real on-air bug, twice); a
handful of statistical-tail chars per 60 s is the price of the 2026-08-07
QRN spike gate, measured on a live lightning night's own capture.

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
  QSO logging = UDP ADIF datagram to cqrlog's bridge on `:2334`;
  Winlink = Pat (getpat.io) -> rigctld `:4532` for QSY + PTT (its long
    `\set_freq`/`\set_ptt` forms, one line back each — `rigctldtest`),
    ardopcf launched from SDR ▾ → Winlink (under fldigi; was AUDIO ▾) on the SignaLink via PipeWire
    (`pipewire:NODE=<node>`, never plughw — it would lock the console
    out), and `PatMonitor` watching Pat's `ws://localhost:8080/ws`: while a
    session dials/connects the console takes DIG and refuses LIVE/RADE/VK/MOX.
  New integrations should follow this pattern, not grab devices.
- **Spot pipeline**: sources (cluster telnet, POTA API, own skimmer) →
  `pushSpots` lambda in MainWindow.cpp → `PanadapterWidget::setSpots`.
  Kinds: 'D' DX yellow, 'P' POTA green, 'F' FT8 cyan, 'S' skimmer violet,
  'V' FreeDV orange (`FreeDvReporter`, qso.freedv.org view-only, Qt WebSockets:
  every online station, every band, in the spot table — ON AIR / TX n min
  ago / listening; only on-air or TX < 20 min reach the band map via
  `SpotLabel::mapHidden`). FreeDV window → Report my station = freedv-gui's
  reporting: role "report" (call/grid/freq/TX/rx_report) + PSK Reporter
  (`PskReporter`, EOO calls, 5-min batches), ONLY in FreeDV mode
  (`updateFreedvLink()` owns the link);
  call text is colored by cqrlog worked-before status (LogbookIndex),
  watch-list hits get an orange ring. While Not1MM runs, its current
  contest's dupes draw gray (`Not1mmLog`: reads not1mm.json + its SQLite
  log READ-ONLY, the contest plugin's own `dupe_type`; SPOTS ▾ toggle) —
  the console's own contest deck, when open, wins.
- **GPU panadapter** (`src/ui/PanGpuView`, QRhiWidget over OpenGL; DISPLAY ▾
  → GPU panadapter, `display/gpuPan`, `TTC_GPUPAN=0/1`; built only with Qt ≥
  6.7 + ShaderTools/GuiPrivate — CI's Ubuntu Qt 6.4 builds the CPU paint;
  the release AppImage uses Qt 6.8 from the official installer since
  v0.1.3 (2026-11-03) so it ships the GPU pan like the desk build, and the
  workflow FAILS if configure says "GPU panadapter: off";
  `-DBUILD_GPU_PAN=OFF` forces CPU paint): the waterfall is a raw-dB
  R32F history texture (palette + view window are shader params), fill/trace/
  3D are GPU geometry, and everything with text is the SAME QPainter code as
  the CPU paint (`paintUnder/Mid/WfLabels/Top`) cached in layer textures,
  redrawn only when `changed()`/`changedTop()`/`changedCursor()` say so — a
  full-screen layer rebuild per spot push was the 5-15 % CPU bug. Shaders in
  `src/ui/shaders`; GLSL `%` of a negative int is UNDEFINED (Mesa disagreed
  with C++: the 3D stack jumped once per ring trip) — keep ring indices
  non-negative. Frame rate (20/30/60, `display/panFps`): the FFT emits on the
  SAMPLE clock (`setSampleClocked`; the IQ arrives in ~43 ms USB bursts, wall-
  time gating gave 23 fps at any setting) and `startSpecPacer()` presents
  frames on a QChronoTimer steered to ~2 queued, consuming extras instead of
  dropping them (61 Hz FFT vs a 60 Hz screen); every time constant (EMA, peak
  droop, waterfall rows/s, 3D scroll) runs off MEASURED time. AetherSDR trace
  look in the DISPLAY panel (all default off/classic): Heat-map style + line
  width, FFT Smooth and De-comb (`src/dsp/TraceShaping`, their GPL-3 code; the
  DCT is our own split-complex FFT — never FFTW's planner on the GUI thread),
  six of their colormaps (palette 4..9, append-only). **3D stacked traces**
  (DISPLAY ▾, `display/pan3d`, GPU only): AetherSDR's real 3DSS, ported
  2026-10-17 — `src/ui/dss` (their DssRenderer row store + test `dsstest`,
  unchanged) and their `dss_mesh` shaders, with the pipeline/mesh/RGBA16F ring/
  UBO hand-ported from their SpectrumWidget into PanGpuView. 96 raw sweeps at
  12.5/s, each with its frequency frame (tuning moves the history with the
  band); the capture beyond the view is their "supplemental" overhang that
  widens the near rows. 3D FLOOR / GAIN / SPAN in the DISPLAY panel
  (`display/dss*`; their 6 / 70 / 100) and "3D passband shading" (their slice-depth decals over VFO A/B, off by default as theirs). The
  tuner card, solar block and compass rose are sidebar boxes in both views
  since 2026-10-18 (`ui/TuneBoxWidget`, `ui/SolarWidget`,
  `ui/CompassRoseWidget` — the contest logger covered them in the
  waterfall's corners); the pan only keeps the QTH (globe backdrop, spot
  bearings → `roseTargetRequested`) and the sun on map backdrops.
  `TTC_PANPROF=1` prints fps / CPU / FFT-dropped-shown / render gaps / pacer
  depth / IQ backlog every 5 s; `TTC_STALLDBG=1|<ms>` backtraces GUI-thread
  stalls; `TTC_PANSPAN=<Hz>` zooms at startup for headless review.
- **SmartMTR** (8th S-meter face, click the meter; `src/ui/SMeterSmartMtr.cpp`,
  AetherSDR's under GPL-3): RX their signal bar, TX PWR (auto 120/600/1200/
  1800 W) + SWR bars and |Z|/phase from the external wattmeter at ~10 Hz
  (`setExtTx`), the radio's @STF without one. 266 px wide — 280 measured
  1862 against the 1850 budget.
- **The skimmer's CW window follows the BAND, not the dial** (2026-10-20):
  `SkimmerEngine::cwSegment()` maps the dial's band to that band's CW
  segment, so working phone up the band keeps the bank (and the SKIM view)
  reading the CW end — the band map is already populated when the operator
  drops down there. Reach is the capture's business: 1 MHz at LO = dial +
  260 kHz covers dial-240k..dial+760k, so from phone it is all of 160/40/
  17/12 m, part of 20/15 m, and none of 75/10 m; `updateFromSpectrum`'s bin
  clamp simply assigns nothing it cannot see, and the SKIM ▾ readout's top
  line says which case the dial is in. (The 760 kHz wasted ABOVE the dial is
  why 75/10 m miss — a dial-dependent LO sign would fix it, untried.)
  `skimtest` asserts the band->segment map and prints the reach table
  (`./skimtest phone` reruns the 10-station segment with the dial up on
  SSB, so the channels sit 350-400 kHz off the LO). SKIM ▾ → "Pause
  outside CW" (2026-10-14) is GONE (2026-10-20): it was what silenced the
  bank on phone ("waterfall shows CW sigs, no decodes" — the STFT view is
  ungated), and working phone is exactly when the band-wide window earns
  its keep. The bank now follows the SKIM toggle alone; `skim/cwOnly` is a
  dead key.

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
  mixer per instance, so banks are cheap (SkimmerEngine runs 24). TWO
  decode brains behind one front end: the legacy battle-tuned slicer
  (every threshold has an on-air story in a comment) and
  `src/cw/FldigiCwEngine` — fldigi's decode logic ported under
  GPL-3 (W1HKJ et al.; keep the attribution header) — used by the CW
  window's tuned reader with FLD/SOM/DEEP/ATK/DCY controls. Full matrix:
  engine 95%, legacy 94% (engine wins sloppy timing via SOM but loses a
  little at 45+ WPM; quick-tier QSB rows under-read the engine — its
  trackers need the full-length runs to warm up, so the quick sentinel is
  `TTC_CWLEGACY=1 cwtest --quick` = 100%; gate: engine full matrix ≥95%).
  `TTC_CWENGINE=1` / `TTC_CWLEGACY=1` force a path in tests (legacy wins
  ties). The ENGINE is the default everywhere (flipped 2026-07-15 after it
  out-copied real fldigi on the same live signal — operator-verified).
  A THIRD brain, `BayesCwEngine` (2026-08-06: two-state HMM soft keying +
  whole-character duration inference), scored 100% on this matrix and was
  removed 2026-10-20 all the same — in two months of on-air chances it
  never earned its keep, so the SKIM menu's A/B toggle, `skim/bayes`,
  `TTC_CWBAYES` and the 514-line engine are gone (git history if ever
  wanted). Synthetic 100% is not an argument against the operator's ears.
  The repo is public and ships GPL-3 because of the fldigi port (fine),
  and released binaries must not bundle the proprietary SDRplay lib.
  Tried and rejected (2026-07-14): an fldigi-style per-element release
  threshold (`off = 0.35 × this element's own peak`). The fast in-key
  peak re-training already collapses the tracker to the current
  element within ~10 ms, so the guarded variant replayed byte-identical
  on every ground-truth capture, and unguarded it merged shallow
  inter-element gaps on clean W1AW (DISPLAYED→DISPLAYEB). Weak-station
  copy is gated by the antenna feed SNR, not element release.
- **SDR receive audio** (SDR ▾ → Receive audio (WDSP), `src/sdr/SdrRx`):
  the RSP2's IQ through WDSP — Thetis's DSP, AetherSDR's vendored 2.10 in
  `third_party/wdsp` (see `TTC-VENDOR.md`; the two NR3 model sources are
  fetched by CMake from TAPR and SHA-checked, not committed). Chain: mix
  the dial to 0 Hz on the IQ worker, FIR-decimate to 125 kHz, WDSP's own
  resampler to 96 kHz (WDSP needs INTEGER rate ratios and nested power-of-
  two block sizes — both ASan-found overruns), conjugate (WDSP keeps HPSDR
  IQ handedness, the mirror of ours), RXA channel 96 k -> 48 k with NB/NB2
  on the IQ, SNB, ANF, NR/NR2/NR3, AGC; RNNoise after, then AetherSDR's RX order:
  gate -> RX EQ (ClientEq, the voice keyer's 8-slot drag curve, stored
  `sdrrx/eq` as VoiceEqSettings JSON) -> compressor -> volume/BOOST ->
  final limiter (-1 dBFS, always on) -> MUTE; thresholds are pre-volume
  dBFS so the volume never moves them; pacat with the LIVE
  mic's backlog control; muted on TX. Follows mode/filter/CW pitch.
  **FreeDV RADE** (window's FREEDV RADE box, `sdrrx/rade`): the USB audio
  is the modem -> `src/sdr/RadeRx` (48k->8k, rade_c `rade_rx`, FARGAN via
  `third_party/rade/ttc_rade_shim`, 16k->48k) -> the same gate/EQ/comp/
  limiter; the decoder is paused while muted/TX and our own call is never
  'heard' (`isOwnCall` — the RSP2 decoded our own EOO and we reported it).
  `third_party/rade` FETCHES rade_c + Opus (SHA-pinned; Arch's
  opus lacks the LPCNet/FARGAN symbols) and commits only the small EOO
  callsign codec (`text/`). `-DBUILD_RADE=OFF` to skip; needs autotools.
  **FreeDV mode** (MODE panel → FreeDV, `setFdvMode`, like SAM a pseudo-
  mode): saves mode/filter/PBT/TX filter/SDR-on/LIVE, sets the band's
  FreeDV sideband (`fdvSideband`: LSB on 160/80/40, USB on 60 m and up —
  qso.freedv.org's Calling Frequencies; NOT 'USB everywhere', live-found
  2026-10-15; follows band changes, USB/LSB buttons flip it inside FDV) +
  2.8 kHz + TX filter >= 2.8 kHz (TX POWER is never touched — operator's
  call, 2026-10-15), arms RADE TX (LIVE
  path, DIG), SDR decodes RADE with NR/NB/ANF/SNB/RNNoise forced off
  (`applySdrRxSettings`, never saved), opens `FreeDvWindow` (sync/SNR,
  Analog, TX level, Report my station, status message, freedv-gui's
  Reporter columns: double-click QSY, right-click rotor/QRZ/QSY request;
  heard log -> Log QSO). Off (or any real mode) restores it all — SSB
  on the CURRENT band's sideband, and the SDR audio always OFF (back to
  the Orion; FDV switches SDR audio without saving `sdrrx/on`). In FDV:
  voice LIVE, VK, over-air replay, DIG-off refused; a Winlink session ends
  FDV. The 30 px FreeDV row made layout-min height 809.
  **RADE transmit** (internal `setRadeTx`, driven by FDV): LIVE in RADE mode —
  mic -> `RadeTx` -> SignaLink, buffer held 130-360 ms (drift drops MIC
  blocks, never modem). Keyed by MOX/space: MOX-off sends the EOO
  (callsign, rade_text) + 200 ms, waits for `radeOverSent()` (2 s
  backstop), THEN drops PTT. Never armed at start-up.
  **Copy Assist** (SDR ▾ → Copy Assist, `src/asr`, AetherSDR's design):
  SdrRx's asr tap (pre gate/EQ/MUTE) -> 16 kHz, noise-floor-tracking VAD
  (quietest-frame-of-2-s floor; pieces 3-7 s: 600 ms quiet, a 250 ms
  phrase pause past 3 s, or a cut at the quietest point at 7 s; queue 2
  deep, newest kept) -> system whisper.cpp base.en on its own thread
  (`ggml_backend_load_all()` first — Arch's ggml has loadable CPU
  variants; this Xeon gets the Sandy Bridge one). Encoder window sized
  to the piece but never < 384 frames (short windows read garbage);
  prompt = a SENTENCE naming our call + calls decoded in the last 3 min
  + band-map calls near the dial (a list prompt echoed back verbatim;
  carrying the previous TEXT made it repeat); max_tokens ~5/s and a
  repeat-collapse stop whisper's '3, 3, 3...' loop. Nonstop talker in
  asrtest: lag ~3 s (was one 20 s lump ~30 s late). Optional: no whisper = no menu
  entry (CI/release build without it).
- **CW deck** (CW PNL sidebar button, `src/cw/CwDeck`, 2026-11-02): the
  contest deck's shape for an everyday CW QSO, in the SAME slot under the
  pan (exclusive with CNTST and RTTY PNL; `TTC_CWDECK=1` opens it headless). Reader
  left, CALL/NAME/S/R/0-BEAT/LOG IT + country line + Spot/SP/LP + F1-F12
  (the CW window's own `cw/fkey/*` macros, F12 = wipe) + WPM/TUNE/STOP +
  FLD/SOM/DEEP/RADIO/NR + the rig's VOL/PITCH/QSK/RISE/KEYER (the radio's
  own keyer speed, `*CS`; never `*CK`) + WinKeyer… in the
  middle, type pane right (Enter keys the line, history above; LIVE/WORD
  under it; right-click sizes either pane, `cwdeck/*Pt`). The deck OWNS
  NOTHING: every send goes through `CwWindow` (`sendTyped`/`streamChars`,
  single master of the WinKeyer), every toggle/slider is a two-way mirror
  of that window's own control (`CwWindow::toggle()/rigSlider()`), the
  QSO is the shared `QsoContext`. ←/→ walk the band map's spots (the
  contest walk, `walkContestSpot`), ↑/↓ change speed from ANY box, the
  type pane included. LOG IT / Enter in S, R or NAME / the CW window's
  new LOG button all call `MainWindow::logFromContext()`: the record +
  dial/mode/UTC + cty into the station log, the same fan-out as the LOG
  window's button (`onQsoLogged`: uploads, cqrlog mirror, QRZ enrichment),
  a 5-minute near-duplicate refused. `[his name]` keys the NAME box's
  word (handle) when there is one, the callbook's otherwise. The UI
  script's `shot <png> <title-word>` grabs a tool window.
- **RTTY** (MODE panel → RTTY + the RTTY PNL deck, `src/rtty`,
  `src/app/MainWindowRtty.cpp`, 2026-11-07): the console's own modem.
  RECEIVE is `RttyDemod` on the RADIO's audio — the SignaLink capture the
  CW reader already owns (`AudioCwSource::setRawTap`), because decode from
  the Orion's ear beats the SDR IQ on air (operator's CW finding; the IQ
  feed is kept in the demod — `process()` takes complex, mark at
  +shift/2 — for a RTTY SKIMMER bank later, not built yet). Chain: two
  recurrence-phasor mixers -> 1 ms boxcar -> 1 kHz -> one-bit boxcar per
  tone (non-coherent FSK matched filter) -> per-tone peak normalisation
  (fldigi's ATC idea, each peak clamped to >= -12 dB of the other or an
  idle sender's leakage reads as a tone) -> UART with a fractional bit
  length, stop bit checked -> ITA2 with USOS. The SQUELCH judges keying
  CONTRAST |m-s|/(m+s) averaged ~1 s (keying 0.6-1.0, noise ~0.4 at any
  level), opens above 0.5 — that is what keeps a dead channel quiet.
  TRANSMIT is `RttyMod` -> pacat into the SignaLink (the DVR's path): AFSK
  on LSB, mark 2125 / space 2295, 170 Hz, 45.45 Bd, 1.5 stop bits, diddle
  (LTRS) while keyed with nothing to say; the engine paces ~140 ms ahead
  of the clock. An OVER = `rttySend`: CAT PTT up, text plays, `end()`
  lets the queue finish, 200 ms mark tail, waits for the pipe to drain,
  THEN `txDrained` drops PTT (the RADE end-of-over pattern); the TX
  button / MOX / space hold the key between lines; Esc aborts. The
  pseudo-mode (`setRttyMode`, FreeDV's shape): saves mode/filter/PBT/TX
  filter/DIG, sets LSB (USB/LSB buttons flip it inside RTTY), TX filter
  >= 2800, DIG, LIVE off; refused under Winlink / WSJT-X TX; refuses VK,
  replay, LIVE, DIG-off while on; off restores (SSB on the band's
  sideband). NARROW = 500 Hz with PBT = (mark+shift/2) - 250 by the
  console's passband model — the nominal low-cut is unreadable, so the
  PBT slider trims it. Logging via `logFromContext` with mode RTTY.
  Calls in "CQ ... DE X" / "N8EM DE X" go to the band map as kind 'R'
  (salmon, 15 min, "calling you" rings orange). RTTY is a per-source
  switch like the rest — SPOTS ▾ "RTTY spots" (`spots/rtty`) for the band
  map, the SPOTS table's RTTY chip for the table — governing every spot
  that says RTTY (cluster comment) plus the deck's own decodes; DX off +
  RTTY on shows RTTY alone. Never an exclusive "RTTY only" filter: the
  one added that day emptied the map after a restart and was pulled the
  same day (the operator mixes the chips himself).
  F1-F12 are `rtty/fkey/*` in the CW window's plain-language fields
  (defaults seeded once). Settings `rtty/*`. `TTC_RTTYDECK=1` opens the
  deck headless. Omni VII: the same AFSK path (its CAT FSK mode is not
  driven). ON AIR 2026-11-07: first QSO KH8A, split 14.097/14.098
  (decode, AFSK TX, PTT, logging all live-verified). The AFC: full-
  passband acquisition of the strongest KEYED pair on its own 8 kHz
  Hann-windowed window, then lock-and-track +-60 Hz, release after 5 s
  quiet; a click locks where it lands. Ground truth lives in
  `~/.local/share/n8mus/tentec-console/audio/` (README there): the
  IK3ORD capture, which `rttytest` decodes from the deck's cold-start
  defaults when present (IK3ORD twice, or FAIL). Still unverified: the
  NARROW PBT offset.
- **One current QSO** (`src/log/QsoContext.h`, 2026-10-19): the station
  being worked — call, handle, name, QTH, grid, RST sent/received — held
  once and shared by the LOG and CW windows, so a call or report typed in
  either is what the other shows and what the CW macros spend. A setter
  that changes nothing emits nothing (that is the whole loop guard).
  **CW macros are plain language** (`src/cw/CwMacros`): `[his call] DE
  [my call] UR RST [rst sent]`, case- and space-tolerant, an unknown
  field left in the text so a typo is visible, and a referenced-but-empty
  field refused by name instead of keying a hole. The old `%mc %c %mn %mq
  %ml %n %r %h` spellings still expand. F1-F12 (`cw/fkey/F1..12` as
  "label|text", three rows of four; the CW1-4 texts are migrated ONCE at
  start-up — doing it lazily per button refresh wrote the old text back
  over a fresh edit, live-found 2026-10-19) key from the keyboard while
  the CW window has focus — `Qt::WindowShortcut`, so the
  contest deck's own F1-F12 never meet them — and the button face is the
  macro's LABEL (CQ / HIS / RST…), its tooltip the expansion as it would
  key right now (cqrlog's CW Type hover). `[my name]` / `[my QTH]` come
  from Setup → Station (`station/name`, `station/qth`). The log's
  **HANDLE** column (schema v3, appended — an ALTER can only append and
  the table model is positional) is the on-air name he gave, kept apart
  from the callbook's NAME and exported as `APP_TTC_HANDLE` by our OWN
  file export only: `toAdif()` is standard ADIF by default, so no
  online log (LoTW / eQSL / QRZ / ClubLog / HRDLog, the HRD and N1MM
  mirrors, the cqrlog bridge) ever receives a field we invented, and
  the handle never stands in for NAME there either (`logtest`).
  **Editing a logged QSO** (Logbook window Edit… / double-click,
  `ui/QsoEditDialog`, 2026-11-07): the table itself is read-only now;
  Save rewrites the record and `QslUploader::repushQso` resets its up_*
  states and re-sends it everywhere (QRZ with `OPTION=REPLACE`, so its
  copy is corrected; eQSL/LoTW/ClubLog/HRDLog and the cqrlog bridge
  dedupe on call/time/band/mode, so a changed key leaves the OLD record
  standing there — the status line says so). `logtest` covers the reset.
- **Radios**: both are carrier-at-dial in CW (no pitch math anywhere).
  Orion = ASCII CAT, dual RX; Omni VII = binary CAT ("omni8"
  personality). Quirks are documented where they're handled — grep
  `live-verified` / `live-found`.

## Hard rules

  open-source attribution (KE9NS/PowerSDR/Thetis) in comments is OK.
- Never key the transmitter or send CW in unattended tests. RX-only use
  of the real SDR/cluster is fine.
- The repo is PUBLIC (alpha since 2026-07-18); pushes happen only after
  the operator's on-air test — build, test, commit, then *wait*.
- Settings are QSettings (`n8mus/tentec-console`) — running the binary
  uses the real config; for tests that must write settings, redirect
  with `XDG_CONFIG_HOME`.
- Never renumber stored enums (meter styles etc.) — append only.

## Station specifics

The operator's own station (radios, serial-port names, keyer, rotor,
wattmeters, cqrlog fork) is described in `CLAUDE.local.md` — private,
git-ignored, not in the public repo. Read it before touching device
paths or hardware behavior.

