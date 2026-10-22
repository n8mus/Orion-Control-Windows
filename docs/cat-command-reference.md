# Ten-Tec CAT command reference

Extracted from the official Ten-Tec programmer's reference guides.

**KEY DESIGN FACT:** Neither radio has a literal Hi-Cut/Lo-Cut command. Both model
the receive filter as **bandwidth + PBT (passband tuning offset)**. Two dragged
panadapter edges `(lo, hi)` map bijectively onto the radio:

    width  = hi - lo        -> set filter bandwidth
    center = (hi + lo) / 2  -> set PBT offset

The driver exposes a single `setPassband(loHz, hiHz)`; each radio's capability
profile quantizes it. See `src/radio/TenTecOrion.cpp` for the Orion implementation.

## Orion 565/566 (firmware v3) — 57600 8N1, direct RS-232, no HW handshake

`*` = set, `?` = query, `@` = response. All commands CR (0x0D) terminated, ASCII.

| Function            | Command                       | Notes                                   |
|---------------------|-------------------------------|-----------------------------------------|
| VFO A/B frequency   | `*AF<hz>` / `*BF<hz>`         | ASCII Hz or MHz; query `?AF`; also 4-byte binary |
| Relative tune       | `*AS+10` / `*BS-22`          | ± tuning steps                          |
| Mode (main / sub)   | `*RMM<0-5>` / `*RSM<0-5>`    | 0=USB 1=LSB 2=UCW 3=LCW 4=AM 5=FM        |
| **Filter bandwidth**| `*RMF<100-6000>` / `*RSF`    | **on-the-fly, 1 Hz resolution**          |
| **Passband (PBT)**  | `*RMP<±8000>` / `*RSP`       | signed ASCII Hz                          |
| AGC                 | `*RMA[F/M/S/P]`              | fast/medium/slow/program                 |
| RF gain / atten     | `*RMG<0-100>` / `*RMT<0/6/12/18dB>` |                                  |
| Antenna select      | `*KA[ant1][ant2][rxant]`     | note the auxiliary RX antenna port       |
| Volume / routing    | `*UM/*US/*UB`, `*UC`         | true dual receiver                       |

Set commands are fire-and-forget (no ACK). Queries return `@`-prefixed lines.
Drag strategy: fire `*RMF`/`*RMP` coalesced to ~20-30/sec; reconcile via periodic query.

### TX metering (?S while transmitting) — real v3 format differs from docs

Live trace: `@STF024R004S608` = fwd 024 W, ref 004 W, **SWR as 8.8 fixed point**
(608/256 = 2.38, matching the SWR computed from those powers). The manual's
example shows a decimal (`S1.1`); the driver accepts both.

### Manual notch — UNDOCUMENTED commands, discovered by live probe (2026-07-10)

In no public Ten-Tec document or hamlib. The V3 user-manual addendum (TT 74474)
documents only `*RMNS` (SAF) but says SAF "uses the same values for NOTCH Center
Frequency and Width parameters. So no new commands are required" — implying these
existed since the V2 firmware. Verified against Jon's Orion 565 (v3 firmware) with
`tools/notch_probe.sh` / `tools/cat_probe -burst`, set + exact read-back:

| Function            | Command                       | Verified behavior                        |
|---------------------|-------------------------------|------------------------------------------|
| Notch center freq   | `*RMNC<hz>` / `?RMNC`         | accepted 20..4000 Hz (1 Hz res); 1, 4500, 9999 **silently rejected**, value unchanged |
| Notch width         | `*RMNW<hz>` / `?RMNW`         | accepted 10..300 Hz incl. 15 (finer than the front panel's 10 Hz steps); 1, 310, 999 rejected |
| Manual notch engage | `*RMNM<0/1>` / `?RMNM`        | clean on/off toggle, query-confirmed     |
| SAF engage          | `*RMNS<0/1>` / `?RMNS`        | documented in V3 addendum; SAF shares CF/W values with notch |

A full `?RMN<A-Z>` letter sweep on the v3 firmware then mapped the whole group:

| Letter | Function                | v3 behavior                                       |
|--------|-------------------------|---------------------------------------------------|
| `A`    | auto-notch level 0-9    | as documented; **queryable** (query undocumented) |
| `B`    | DSP noise blanker 0-9   | as documented; **queryable**                      |
| `N`    | NR per rev 1.2 docs     | **DEAD on v3** — set silently ignored, query unanswered |
| `R`    | **noise reduction 0-9** | v3 replacement for `N`; set/read-back verified    |
| `H`    | hardware NB on/off      | undocumented; accepts only 0/1 (5 rejected) — presumed the IF hardware blanker |
| `C/W/M/S` | manual notch (above) |                                                   |

Sub receiver presumably `*RSN<same letters>` (untested). Out-of-range sets are
REJECTED, not clamped — the UI must clamp before sending or the radio ignores
the command. This unlocks the draggable on-screen notch marker (same pattern
as the passband drag: `*RMNC` streams the drag, `*RMNW` from a width gesture,
audio-offset → RF mapping must respect mode sideband like PBT does).

## Omni VII 588 — 57600 8N1, **hardware handshaking (RTS/CTS) required**; also Ethernet

Two operating modes: RADIO MODE (subset, compatible with existing programs) and
REMOTE MODE (full command set + Ethernet control + audio streaming/RIP).

| Function            | Command                       | Notes                                   |
|---------------------|-------------------------------|-----------------------------------------|
| VFO A/B frequency   | `*A<4-byte bin>` or `*A14.250`| query `?A`                              |
| Mode                | `*M<A><B>`                    | ASCII '0'..'6' = AM/USB/LSB/CWU/CWL/FM/FSK |
| **Passband (PBT)**  | `*P<d1><d0>`                  | 2-byte binary, ±8192 Hz, 0=off           |
| **RX filter**       | `*W<d0>`                      | d0 = 0..0x24 → one of **37 fixed presets** (14000…200 Hz) |
| I-F roofing select  | `*C1T<0-5>` / enable `*C1U`   |                                          |
| Sideband TX BW      | `*C1O`                        | 1000-4000 Hz                             |

Consequence: on the Omni VII the passband drag is **stepped** (snap to nearest of
37 widths + PBT re-center), not smooth. The Orion is the flagship for the feature.
