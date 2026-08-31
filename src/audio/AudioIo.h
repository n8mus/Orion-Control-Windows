// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>
#include <vector>

class QAudioSource;
class QAudioSink;
class QIODevice;

namespace ttc {

// Qt Multimedia capture/playback endpoints for the Windows build (compiled
// wherever Qt6Multimedia exists, used where the PipeWire CLI tools don't).
// The Linux paths keep parec/pacat on purpose — their pulse-layer choice is
// live-found (native-client wedges, monitor levels ~22 dB low) and fldigi
// shares the device through PipeWire. WASAPI shared mode gives Windows the
// same multi-reader property.
//
// Both classes speak s16le MONO at the caller's rate regardless of what the
// device actually runs: the device opens at whatever format it supports
// (Int16/Float, mono/stereo, its own rate) and conversion happens here —
// downmix, sample-format, and stateful linear resampling. That is what
// keeps the Omni's 7013 Hz RIP/TRIP rate and the decoder's 48 kHz honest
// on backends that only do the shared-mix rate.

// Stateful linear resampler for s16 mono streams (float accumulator, keeps
// phase and the last sample across chunks).
class LinearResampler {
public:
    void reset(double srcRate, double dstRate);
    // Append converted samples to out. Pass-through when rates match.
    void process(const int16_t* in, size_t n, std::vector<int16_t>& out);

private:
    double step_ = 1.0;                    // src samples per dst sample
    double pos_ = 0.0;                     // fractional read position
    int16_t last_ = 0;                     // final sample of previous chunk
    bool primed_ = false;
};

// Capture: device → chunk(s16le mono @ rateHz) signals.
class AudioCapture : public QObject {
    Q_OBJECT
public:
    explicit AudioCapture(QObject* parent = nullptr);
    ~AudioCapture() override;

    // deviceMatch: case-insensitive substring of the input's description
    // ("USB AUDIO CODEC" finds the SignaLink); empty = system default.
    bool start(const QString& deviceMatch, int rateHz);
    void stop();
    bool running() const { return src_ != nullptr; }

    // Description of the device actually opened (after matching), for
    // status lines. Empty when stopped.
    QString deviceDescription() const { return desc_; }

    static QStringList inputDescriptions();

signals:
    void chunk(const QByteArray& s16leMono);
    void errorText(const QString& what);

private:
    void onReadable();

    QAudioSource* src_ = nullptr;
    QIODevice* io_ = nullptr;
    QString desc_;
    int dstRate_ = 48000;
    int srcRate_ = 48000;
    int srcChannels_ = 1;
    int srcSampleFmt_ = 0;                 // QAudioFormat::SampleFormat
    QByteArray carry_;                     // partial frame between reads
    LinearResampler resamp_;
    std::vector<int16_t> monoBuf_;         // scratch: downmixed input
    std::vector<int16_t> outBuf_;          // scratch: resampled output
};

// Playback: write(s16le mono @ rateHz) → device.
class AudioPlayback : public QObject {
    Q_OBJECT
public:
    explicit AudioPlayback(QObject* parent = nullptr);
    ~AudioPlayback() override;

    // deviceMatch: substring of the output's description; empty = default.
    bool start(const QString& deviceMatch, int rateHz);
    void stop();                           // drains what fits, then closes
    bool running() const { return sink_ != nullptr; }

    void write(const QByteArray& s16leMono);

    // Flow control for file playback: how many CALLER-rate s16 mono bytes
    // currently fit (device free space scaled back through the conversion),
    // and whether the sink has drained everything written so far.
    qint64 bytesFree() const;
    bool drained() const;

    static QStringList outputDescriptions();

signals:
    void errorText(const QString& what);

private:
    QAudioSink* sink_ = nullptr;
    QIODevice* io_ = nullptr;
    int srcRate_ = 48000;                  // caller's rate
    int dstRate_ = 48000;                  // device rate
    int dstChannels_ = 1;
    int dstSampleFmt_ = 0;
    QByteArray carry_;
    LinearResampler resamp_;
    std::vector<int16_t> outBuf_;
};

} // namespace ttc
