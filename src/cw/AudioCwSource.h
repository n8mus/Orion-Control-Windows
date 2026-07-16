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

signals:
    void statusChanged(const QString& text);

private:
    void onReadable();

    CwDecoder* sink_;
    QProcess* proc_ = nullptr;
    QByteArray carry_;                     // odd trailing byte between reads
    std::vector<std::complex<float>> buf_;
    bool nrOn_ = false;
    DenoiseState* nrSt_ = nullptr;         // lazily created, reset on start
    std::vector<float> nrIn_;              // 480-sample frame accumulator
    int nrFill_ = 0;
};

} // namespace ttc
