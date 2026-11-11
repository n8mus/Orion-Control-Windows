// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QByteArray>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QStringList>

#include <atomic>
#include <memory>
#include <vector>

class QAudioSource;
class QAudioSink;
class QIODevice;
class QThread;
class QTimer;

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
    // bufferMs: the device queue. 300 rides network/file jitter; live
    // streams whose delay the operator hears (the LIVE mic, SDR receive)
    // ask for a few tens of ms through PcmSink.
    // nativeFormat: open the device in its OWN format (the shared-mode mix
    // format) and convert here, instead of asking Qt for s16 mono at our
    // rate. Qt then runs no converter of its own. PcmSink uses this: fed
    // small irregular chunks from a worker thread, Qt's Media Foundation
    // resampler stopped taking input for good ("Resampling failed
    // -1072875851", MF_E_NOTACCEPTING; live-found 2026-11-11).
    bool start(const QString& deviceMatch, int rateHz, int bufferMs = 300,
               bool nativeFormat = false);
    void stop();                           // drains what fits, then closes
    bool running() const { return sink_ != nullptr; }

    void write(const QByteArray& s16leMono);

    // Flow control for file playback: how many CALLER-rate s16 mono bytes
    // currently fit (device free space scaled back through the conversion),
    // and whether the sink has drained everything written so far.
    qint64 bytesFree() const;
    bool drained() const;
    // CALLER-rate s16 mono bytes sitting in the device queue, not yet played.
    qint64 bytesQueued() const;

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

// Queue-fed playback with a measurable backlog: what pacat on a pipe is to
// the Linux paths. write() never blocks and may come from any thread; a
// short timer moves what fits into the device. The producer paces itself
// against backlogBytes() (queue + device, caller-rate bytes), which is how
// the LIVE mic and SDR receive hold their delay when the two sound clocks
// drift apart.
//
// useOwnThread() runs the device on a private thread: for SDR receive,
// whose audio must not stutter when the GUI thread is busy painting.
class PcmSink : public QObject {
    Q_OBJECT
public:
    explicit PcmSink(QObject* parent = nullptr);
    ~PcmSink() override;

    void useOwnThread();                   // before start()
    // queueCapMs: the most write() will hold ahead of the device (a stalled
    // device must not grow it forever). Raise it for a whole clip handed
    // over in one write.
    bool start(const QString& deviceMatch, int rateHz, int deviceBufferMs = 40,
               int queueCapMs = 2000);
    void stop();
    bool running() const { return running_.load(); }

    void write(const char* s16leMono, qint64 bytes);      // any thread
    void write(const QByteArray& b) { write(b.constData(), b.size()); }
    qint64 backlogBytes() const;                          // any thread
    // Only what is still waiting to enter the device. This is the figure a
    // producer steers by: it is what the pipe in front of pacat holds on
    // Linux (the device's own buffer is the player's business there too),
    // so the same targets mean the same thing on both.
    qint64 queuedBytes() const { return fifoBytes_.load(); }

signals:
    void errorText(const QString& what);

private:
    void pump();                           // device thread
    template <typename F> void onDeviceThread(F&& f);

    QObject* ctx_ = nullptr;               // lives on the device thread
    QThread* thread_ = nullptr;
    AudioPlayback* pb_ = nullptr;          // device thread only
    QTimer* timer_ = nullptr;              // device thread only
    mutable QMutex mux_;
    QByteArray fifo_;                      // under mux_
    qint64 capBytes_ = 0;
    qint64 minChunk_ = 0;                  // device takes pieces this big or more
    std::atomic<qint64> fifoBytes_{0}, devBytes_{0};
    std::atomic<bool> running_{false};
};

} // namespace ttc
