// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QObject>
#include <QString>
#include <complex>
#include <vector>

class QProcess;
struct DenoiseState;

namespace ttc {

class CwDecoder;
class AudioCapture;

// Radio-audio source for the CW window's tuned reader: captures the
// SignaLink (the Orion's audio out — real antenna, crystal filter, AGC)
// via PipeWire's `parec` and feeds it to a CwDecoder as complex samples
// with imag=0. The decoder's mixer sits on the CW sidetone pitch
// (cw/pitchHz, 550), so the whole existing chain — FIR, rx-AGC, fldigi
// engine, AFC — runs unchanged on the radio's ears instead of the SDR's.
// This is the input real fldigi gets, which is why fldigi won on weak
// signals: the ~15 dB passive-tap deficit lives in the SDR feed, not in
// anyone's decoder (operator-driven insight, 2026-07-15). parec was
// chosen over Qt Multimedia deliberately: no new library dependency, and
// PipeWire lets fldigi keep reading the same source simultaneously.
class AudioCwSource : public QObject {
    Q_OBJECT
public:
    explicit AudioCwSource(CwDecoder* sink, QObject* parent = nullptr);

    void start();                          // spawn parec (idempotent)
    void stop();
    bool running() const;

    // RNNoise ahead of the decoder. Ruler verdict (tests/nrtest,
    // 2026-07-16): perfect copy to -6 dB SNR where raw audio busts and a
    // tone-tuned spectral gate barely helps — the speech-trained net
    // protects a tonal carrier beautifully. ~10 ms latency.
    void setNr(bool on);

    // The operator's sidetone pitch, so the parked-carrier notch never
    // convicts it. Holding the radio's SPOT looks EXACTLY like a birdie —
    // rock-steady tone, steady level — so three SPOT readings taught the
    // tracker to exclude the one frequency this meter exists to measure,
    // and copying a station there kept the conviction alive forever
    // (live-found 2026-08-23: "bouncing orange numbers", cured by a
    // restart because the conviction isn't persisted).
    void setTargetPitch(int hz);

signals:
    void statusChanged(const QString& text);
    // Strongest audio tone 200-1200 Hz, ~4x/s, parabolic-interpolated
    // (about +/-1 Hz) — the fldigi-equivalent pitch readout. -1 = no
    // signal standing above the floor (or capture stopped). Measured on
    // the RAW stream, before NR.
    void pitchMeasured(double hz);

private:
    void onReadable();
    // Shared tail of the pipeline: raw s16le mono 48 kHz bytes in (from
    // parec on Linux, AudioCapture on Windows), decoder/pitch/NR out.
    void processPcm(const QByteArray& data);

    CwDecoder* sink_;
    QProcess* proc_ = nullptr;
    AudioCapture* cap_ = nullptr;          // Windows capture endpoint
    QByteArray carry_;                     // odd trailing byte between reads
    std::vector<std::complex<float>> buf_;
    bool nrOn_ = false;
    DenoiseState* nrSt_ = nullptr;         // lazily created, reset on start
    std::vector<float> nrIn_;              // 480-sample frame accumulator
    int nrFill_ = 0;
    // pitch meter (see pitchMeasured)
    void measurePitch();
    std::vector<float> pitchBuf_;          // rolling window, kPitchN samples
    int pitchFill_ = 0;                    // samples since last measurement
    // constant-carrier rejection (see measurePitch): a tone that holds
    // frequency AND level dead-steady for ~8 s is an artifact, not keyed
    // CW, and is notched out of the peak search.
    double steadyHz_ = -1.0;               // candidate tone being tracked
    double steadyDb_ = 0.0;                // its level last window
    int    steadyN_ = 0;                   // consecutive steady windows
    double notchHz_ = -1.0;                // convicted carrier (<0 = none)
    int    notchGoneN_ = 0;                // windows since it vanished
    int    targetHz_ = 0;                  // sidetone pitch, never notched
};

} // namespace ttc
