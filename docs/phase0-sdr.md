# Phase 0 — SDRplay RSP2 streaming (verified live)

Implemented `src/sdr/SdrPlaySource.cpp` against the SDRplay API v3.15 and verified
end-to-end on the live RSP2 (`1df7:3010`) with `tools/sdr_probe`, 2026-07-10.
Fed from the Orion's spare-antenna jack (TX-isolated), centered on 40 m.

## Configuration used

Zero-IF, 1.536 MHz analog BW, 2.0 MS/s, fixed gain (AGC off), Antenna A.

## Result

```
center 7.1500 MHz, requested fs 2.0 MS/s
samples 3,878,784 in 2.0s  =>  1.939 MS/s achieved   (matches request)
API version 3.15
```

## Key finding: the shared-antenna feed is HOT

The whole 40 m band arrives at the RSP2, so default gain overloads the ADC:

| gRdB | LNAstate | mean power | peak power | verdict          |
|------|----------|-----------|-----------|------------------|
| 40   | 0        | −6.1 dBFS | +0.7 dBFS | **ADC overload / clipping** |
| 40   | 6        | −44.7 dBFS| −21.7 dBFS| **healthy — chosen default** |
| 50   | 8        | −62.3 dBFS| −33.1 dBFS| conservative     |

`SdrPlaySource` now defaults to **gRdB 40 / LNAstate 6**. A single overload event
fires during `Init` gain-settling; steady state is clean. `setGain(gRdB, lnaState)`
overrides. When the panadapter UI lands, expose gain as a slider and watch the
`PowerOverloadChange` event to warn/auto-attenuate.

## Streaming API path (for reference)

Open → LockDeviceApi → GetDevices (match `SDRPLAY_RSP2_ID`) → SelectDevice →
UnlockDeviceApi → GetDeviceParams → set fs/rf/bw/if/gain/antenna → Init(callbacks) →
stream. `setCenterFrequency` uses `Update(Tuner_A, Update_Tuner_Frf)`. The stream
callback normalizes short I/Q by 1/32768 into `std::complex<float>` and runs on the
SDRplay thread — GUI consumers must marshal to the UI thread (queued signal).

## Display path (done + verified)

IQ → `SpectrumComputer` (Hann window → radix-2 `Fft` → power dB → fftshift →
temporal smoothing, throttled to ~20 fps) → marshaled to the GUI thread via
`QMetaObject::invokeMethod(..., QueuedConnection)` → `PanadapterWidget::setSpectrum`.
Displayed span is decimated to 250 kHz (`setDecimation(8)`), centered on the dial.

Verified headless (`QT_QPA_PLATFORM=offscreen TTC_SELFTEST=5 ./tentec-console`):
live 2048-bin frames, ~40 dB SNR (peak −74 dB over a −115 dB floor), clean exit.

**Concurrency note (fixed):** the SDRplay callback runs on its own thread and
references `spectrum_`. Member teardown order would free the DSP members before
`sdr_` stops, causing a use-after-free in `Fft::forward` at shutdown. `~MainWindow`
now calls `sdr_.stop()` first (Uninit drains callbacks) before those members die.

## Next

FFT → WDSP demod for the operator's SDR RX audio, and align the panadapter
filter-drag overlay with the live spectrum (zoom to the passband for editing).
