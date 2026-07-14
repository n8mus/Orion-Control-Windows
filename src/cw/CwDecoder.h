// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QObject>
#include <atomic>
#include <complex>
#include <cstddef>

namespace ttc {

// CW reader fed straight from the SDR IQ stream — no audio cable, works
// with the AF gain at zero. The tuned station's carrier sits at a FIXED
// offset from the SDR LO (-kLoOffsetHz, because the capture LO rides
// 60 kHz above the dial and both radios read the carrier on the dial in
// CW), so the mixer never needs retuning: CW zap puts the carrier at the
// dial, the dial is always -60 kHz from the LO, and this decoder listens
// exactly there in a ~±110 Hz window.
//
// Chain (input 500 ksps, SDR thread): mix +60 kHz to DC -> 250:1 boxcar
// decimate (2 ksps) -> 8-tap complex moving average (~±110 Hz) ->
// magnitude envelope -> adaptive two-level slicer (tracks signal peak and
// noise floor separately) -> mark/space run lengths -> adaptive-dit Morse
// classifier. Characters are emitted via queued signal to the GUI.
class CwDecoder : public QObject {
    Q_OBJECT
public:
    explicit CwDecoder(double inputRate, double offsetHz,
                       QObject* parent = nullptr);

    void setEnabled(bool on);              // atomic; safe from any thread
    bool enabled() const { return enabled_.load(std::memory_order_relaxed); }

    // Move the listening window to a new offset from the SDR LO (skimmer
    // channels hop between stations). Safe from any thread; the SDR thread
    // applies it at the next block and resets all demod state.
    void retune(double offsetHz);

    // AFC: how far the mixer has been steered off the assigned offset to
    // stay centered on the station (Hz; reads from any thread).
    double afcHz() const { return afcTotal_.load(std::memory_order_relaxed); }

    // SDR streaming thread.
    void processIq(const std::complex<float>* d, size_t n);

signals:
    void textDecoded(const QString& text); // one or more characters
    void wpmEstimated(int wpm);

private:
    void tick(float mag);                  // one envelope sample (2 kHz)
    void classifyGap(double gapMs);
    void emitSymbol();

    std::atomic<bool> enabled_{false};
    std::atomic<double> pendingOffset_{0.0};
    std::atomic<bool> retunePending_{false};
    double inputRate_ = 500000.0;

    // mixer + decimator (SDR thread only). The LO is a recurrence phasor
    // (one complex multiply per sample, renormalized periodically) — a
    // skimmer bank of these runs where per-sample sin/cos would not.
    std::complex<double> lo_{1.0, 0.0}, loStep_{1.0, 0.0};
    int renorm_ = 0;
    std::complex<double> acc_{0.0, 0.0};
    int accN_ = 0, decim_ = 250;
    // Post-decimation moving average: 8 taps (~±110 Hz) normally, dropped
    // to 4 (~±220 Hz) at high speed — 27 ms elements at 45 WPM disappear
    // into an 8 ms filter+envelope smear otherwise.
    std::complex<float> ma_[8] = {};
    int maIdx_ = 0;
    int maLen_ = 8;

    // AFC (SDR thread): quadrature discriminator on the decimated samples,
    // measured only while the key is down (that's when there's a carrier
    // to measure). The mixer is nudged every ~100 ms, bounded to ±80 Hz
    // total so a channel can never wander onto its neighbor. This is what
    // keeps a slightly-off assignment (or a drifting sender) centered in
    // the ±110 Hz matched filter instead of sliding down its skirt.
    std::complex<float> afcPrevZ_{0.0f, 0.0f};
    double afcErrAvg_ = 0.0;               // EMA of measured error, Hz
    int    afcTicks_ = 0;                  // key-down ticks since last nudge
    std::atomic<double> afcTotal_{0.0};    // applied correction, Hz

    // envelope + slicer
    float env_ = 0.0f;                     // smoothed magnitude
    float peak_ = 0.0f, floor_ = 1e-3f;    // adaptive signal/noise trackers
    bool  key_ = false;
    double runMs_ = 0.0;                   // current mark/space duration
    double tickMs_ = 0.5;                  // 2 kHz envelope rate

    // Morse timing. ditMs_ is the adaptive dit clock; gapMin_ is a
    // decaying minimum of observed space lengths — the element gap IS one
    // dit of the sender's clock, and short gaps keep leaking through even
    // when high-speed marks smear together, so it's the bootstrap that
    // breaks the 45+ WPM acquisition deadlock (slow filters -> merged
    // marks -> clock stuck high -> filters stay slow).
    double ditMs_ = 60.0;                  // adaptive (starts ~20 WPM)
    double gapMin_ = 60.0;                 // decaying min of space lengths
    double lastSpaceMs_ = 0.0;             // for bridging over noise blips
    QString sym_;                          // dits/dahs of the current char
    bool wordGapSent_ = true;
    int lastWpm_ = 0;
};

} // namespace ttc
