# WSJT-X from the console (replacing cqrlog's "Remote mode WSJT-X")

Report, 2026-10-22. Status: proposal, nothing built yet.

## 1. What cqrlog does today

cqrlog's **Remote → WSJT-X** (`fNewQSO.pas` `tmrWsjtxTimer`, plus the
2,677-line `fMonWsjtx.pas` "CQ monitor") is a **UDP client of WSJT-X's
message protocol** (NetworkMessage.hpp: magic `0xadbccbda`, Qt
QDataStream, big-endian). It:

1. **Reads** the stream WSJT-X sends:
   - Heartbeat (0)
   - Status (1): dial, mode, DX call, TX enable/transmitting
   - Decode (2): every decoded line
   - Clear (3)
   - QSOLogged (5) / LoggedADIF (12)
2. **Fills its New QSO window** from Status (call, grid, mode, band),
   looks up the callsign, and logs the QSO when WSJT-X sends 5/12.
3. **Shows the CQ monitor**: decodes coloured by worked-before /
   new-DXCC / new-grid / US state. Double-clicking a line **sends a Reply
   (4)** back to WSJT-X, which starts the standard QSO exactly as a
   double-click inside WSJT-X would.
4. **Sends commands back**:
   - FreeText (9)
   - Configure (15): mode, DX call/grid, GenStdMsgs
   - Clear, HighlightCallsign (13), HaltTx (8)
5. **Optionally launches wsjtx** for you.

## 2. What the console already has (more than you'd think)

| piece | where | state |
|---|---|---|
| rig control for WSJT-X | `RigctldServer` :4532 (`CATNetworkPort=127.0.0.1:4532`, PTT = CAT) | **working** — WSJT-X already runs through the console, not cqrlog |
| WSJT-X UDP listener | `src/log/WsjtxListener` | reads Status (DX call → New QSO + globe) and LoggedADIF (12) → logbook with dupe check, cty fill, online push |
| FT8 spots | cluster feed, cyan 'F' labels | working (others' decodes, not ours) |
| worked-before colouring | `LogbookIndex` | working, reusable for decodes |

## 3. The gap (corrected 2026-10-22)

WSJT-X sends to 127.0.0.1:2238, which is **GridTracker2's** port, not
cqrlog's. GridTracker forwards everything to 127.0.0.1:2237, where the
console's listener (bound to any address, port 2237) picks it up. So
FT8 QSOs reach the console **only while GridTracker2 is running**.
GridTracker also uploads each QSO to LoTW/QRZ/ClubLog/eQSL itself
(`logLOTWqsoCheckBox` etc. on), and the console pushes too
(`log/wsjtxPush`=true), so every FT8 QSO is posted twice. The online
logs mostly reject the second copy, but one of the two pushes should be
turned off.

What's missing for real operating is everything that **talks back**:
the decode list, click-to-call, and the controls.

## 4. Proposed design — "FT8 deck" in the console

The console follows its usual pattern: it is the single master of each
shared resource. WSJT-X keeps doing what only it can do (the modem,
decoding and its sequencer). We don't reimplement FT8. The console
becomes WSJT-X's **controller and display**, the way it already is its
rig.

### 4.1 `WsjtxLink` (grow `WsjtxListener` into it, `src/net/`)
- Parse every inbound type:
  - 0 Heartbeat: alive / version
  - 1 Status: full struct, incl. TX enabled, transmitting, Tx/Rx DF,
    DE call, sub-mode, `tx watchdog`, `special op mode`
  - 2 Decode, 3 Clear, 5 QSOLogged, 6 Close, 7 Replay response,
    10 WSPRDecode, 12 LoggedADIF
- **Remember the sender's address and port** from the first datagram.
  Replies go there. That's how WSJT-X expects clients to answer, and
  multicast works the same way.
- Send:
  - 4 Reply: the double-click equivalent; it must echo the decode's
    time/snr/dt/df/mode/message exactly
  - 7 Replay: ask WSJT-X to resend this period's decodes when the deck
    opens
  - 8 HaltTx: auto-only or immediate
  - 9 FreeText (+send)
  - 13 HighlightCallsign: colour WSJT-X's own Band Activity with OUR
    worked-before status
  - 15 Configure: mode, DX call, grid, GenStdMsgs, Rx/Tx DF
  - 16 SwitchConfiguration
- Pure codec + socket, testable offline like `rigctldtest`/`psktest`:
  **`wsjtxtest`** builds every message byte-for-byte against
  NetworkMessage.hpp's layout, round-trips them, and runs a fake WSJT-X
  peer on a local port.

### 4.2 FT8 deck window (`src/ui/WsjtxWindow`)
cqrlog's CQ monitor, done the console's way:
- **Decode table** for the current period: UTC, dB, DT, DF, message,
  plus columns the console already knows: country, distance and bearing
  from EN83al, worked-before colour (`LogbookIndex`: new DXCC / new band
  / new grid / worked), watch-list ring, Not1MM dupe grey.
  Filters: CQ only / calling me / new-only / band.
- **Double-click = Reply (4)**. WSJT-X then runs the sequence. "Calling
  me" lines get flagged in red, like WSJT-X's own.
- **Status strip**: mode, dial, TX enabled / TRANSMITTING, Tx/Rx DF,
  DX call, buttons for HALT TX (8) / Enable TX / free text.
- QSO done (12) → logged as today. The current QSO feeds `QsoContext`,
  so the LOG window shows the station being worked live and the logbook
  is the record. **This replaces cqrlog's New-QSO role.**
- A sidebar button **FT8** built in `MainWindowTools.cpp` (`setupWsjtxUi()`
  + `topLay2_`), lit while WSJT-X is heard (`lightWhile`). That costs no
  width (sidebar grid rule).

### 4.3 Panadapter / band map integration (cheap wins)
- Our own decodes as band-map spots: a new kind, or reuse 'F' with a
  "local" flag, placed at dial + DF. Click a label → Reply.
- Mark WSJT-X's Tx and Rx DF on the pan as two thin markers in the
  passband, so the waterfall shows where we'll transmit. Click-drag on
  the pan → Configure(15) Tx DF. Find a clear slot by eye on the RSP2's
  better waterfall.
- Rose/rotor: the DX grid is already on the globe through `dxChanged`;
  add "turn the beam" from the decode row's right-click, like the
  FreeDV table.

### 4.4 Launching and supervising WSJT-X
- SDR ▾ (or the FT8 button menu) → **Start WSJT-X**: `QProcess` `wsjtx`
  (optionally `--rig-name=` for a config per radio, Orion vs Omni VII).
  Watch Heartbeat / Close(6) for the light.
- Same pattern as ardopcf/Winlink: while WSJT-X is transmitting
  (Status.transmitting, or rigctld PTT from it), the console **takes DIG
  and refuses LIVE / VK / MOX / CW**, mirroring the `PatMonitor` session
  lock. On start, warn if the SignaLink route isn't DIG.
- Audio stays on the SignaLink through PipeWire, as now. No device
  grabbing.

## 5. Safety rules carried over
- **Never key in tests.** `wsjtxtest` uses a fake peer. Any live check
  runs with WSJT-X's "Enable Tx" off, and HaltTx is sent on deck close /
  console quit.
- Reply only sends what WSJT-X decoded. WSJT-X keeps its own TX watchdog
  and sequencer, so the console never generates FT8 audio.
- The console stays the logger of record. Dupes are guarded by
  `hasNearDuplicate` (WSJT-X re-sends 5 and 12 for the same QSO).

## 6. Suggested phases

| phase | what | size |
|---|---|---|
| 0 | change the WSJT-X UDP target (§3), **before retiring cqrlog** | 2 min, settings only |
| 1 | `WsjtxLink` codec + sender-address reply path + `wsjtxtest` | ~1 day |
| 2 | FT8 deck: decode table, colours, double-click Reply, HALT, status | ~1–2 days |
| 3 | HighlightCallsign back into WSJT-X, decodes on the band map, DF markers | ~1 day |
| 4 | Start/supervise WSJT-X, DIG lock / refusal while it transmits | ~½ day |
| 5 | extras: auto-reply to new-DXCC CQs (opt-in, with a watchdog), WSPR decodes, JTDX/MSHV compatibility (same protocol) | later |

## 7. Open questions for Jon
1. Multicast 224.0.0.1:2237 (other apps like GridTracker can listen
   too) or unicast to the console only?
2. Should the console start WSJT-X itself, or keep launching it by hand?
3. Decodes on the panadapter, or keep them in the deck only (busy
   FT8 segments can be 40+ calls)?
4. Any of cqrlog's CQ-monitor features you relied on that aren't above
   (US-state alerts, "follow call", alert sounds)?
