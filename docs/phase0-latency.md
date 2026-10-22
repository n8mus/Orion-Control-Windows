# Phase 0 — Orion CAT latency measurement

Measured 2026-07-10 against the live Orion (firmware v3) on `/dev/orion`
(→ `/dev/ttyUSB0`, an **FTDI FT4232H** USB-serial bridge), 57600 8N1, read-only
queries via `tools/latency_probe.c`. 50 samples each.

Radio confirmed live: main filter 3000 Hz, VFO-A on 7.1119 MHz.

## Results

| query  | FTDI timer | min    | median | mean   | p90     | max     |
|--------|-----------|--------|--------|--------|---------|---------|
| `?RMF` | 16 ms     | 10.9ms | 26.9ms | 47.8ms | 107.0ms | 122.9ms |
| `?RMF` | **1 ms**  | 9.9ms  | **16.8ms** | 35.8ms | 110.9ms | 113.9ms |
| `?AF`  | 16 ms     | 26.8ms | 26.9ms | 55.2ms | 122.9ms | 123.0ms |
| `?AF`  | **1 ms**  | 13.0ms | **19.9ms** | 39.9ms | 110.0ms | 114.9ms |

## Interpretation

- Round-trip is **bimodal**: a fast cluster and a consistent tail near ~110 ms.
- Lowering the FTDI latency timer 16 ms → 1 ms cut the **median ~27 → ~17 ms** and
  the floor, but left the ~110 ms tail essentially unchanged (max ~114 ms both runs).
- Therefore the tail is **not** the FTDI adapter — it is the **Orion firmware**
  servicing its UART on a periodic (~100 ms) housekeeping cycle, so a query landing
  just after a service tick waits nearly a full cycle. This is intrinsic to the
  radio; no host-side tuning removes it. The 57600 link itself is negligible
  (~1.5 ms of wire time per short command).
- **This does not gate the drag-to-filter feature.** Orion `*RMF`/`*RMP` sets are
  fire-and-forget (no ACK), so a drag just *streams* set commands (~1.5 ms TX each),
  coalesced to ~20-30/sec — well within budget. The round-trip figures above only
  bound how fast a *reconciliation query* confirms state, not the filter response.

## Design consequences

- **Set FTDI latency_timer=1 ms** anyway — it's a free ~10 ms off the median/floor.
  Persist it (udev): `/etc/udev/rules.d/99-tentec-ftdi.rules`
  `SUBSYSTEM=="usb-serial", DRIVER=="ftdi_sio", ATTR{latency_timer}="1"`
  then `sudo udevadm control --reload && sudo udevadm trigger`.
- **Drag-to-filter path:** unaffected. `*RMF`/`*RMP` sets are fire-and-forget, so a
  drag streams writes (~1.5 ms TX each) coalesced to ~20-30/sec; the radio applies
  them on its own cycle and we never block on a round trip.
- **Reconciliation/readback path:** must be fully async (our `SerialPort` already is,
  via `QSocketNotifier` + signals) and poll modestly (~5-10 Hz). Never block the UI
  waiting on a query — budget for occasional ~110 ms stragglers.

> Note: the Orion reaches the PC via an FTDI FT4232H, i.e. a USB-serial bridge —
> worth flagging since the earlier assumption was a native RS-232 UART. Doesn't
> change the driver, but the FTDI timer tuning above is specific to it.
