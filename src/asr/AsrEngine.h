// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "asr/CallExtract.h"

struct whisper_context;

namespace ttc {

// Copy Assist: speech-to-text on the SDR receive audio, AetherSDR's
// design (src/asr, GPL-3, KK7GWY and contributors) on whisper.cpp (MIT;
// Arch's whisper-cpp, whose ggml picks a CPU variant this AVX-only Xeon
// can run). One whisper run per closed utterance — no sliding window:
//
//   SDR DSP thread  feed48k(): 3:1 FIR to 16 kHz, 10 ms frames, VAD,
//                   close a piece on silence / phrase pause / TX, cut at
//                   7 s at its quietest point -> queue (2 deep, newest kept)
//   own thread      whisper_full() per utterance -> transcribed()
//
// The VAD tracks the noise floor (quietest frame of 2 s) rather than
// AetherSDR's fixed level: after WDSP's AGC the hiss between words sits
// far above any absolute threshold. Pieces stay 3-7 s (live-found: a
// talker who never paused made one 20 s lump, decoded 30 s late and
// worse). Two ttc additions, both measured here (base.en, 8 threads,
// a 9.8 s clip): the encoder window is sized to the utterance (whisper
// pads every call to 30 s — audio_ctx cut 9.5 s to 4.3 s), and the
// prompt carries the calls spotted near the dial (a generic ham prompt
// changed nothing; with the right call in it "G-O-D-S-E-Z" became
// "G8SEZ").
class AsrEngine : public QObject {
    Q_OBJECT
public:
    struct Result {
        QString text;
        float confidence = 0.f;                    // mean token probability
        qint64 atMs = 0;                           // utterance start, epoch ms
        double seconds = 0;                        // utterance length
        int decodeMs = 0;
        qint64 dialHz = 0;
        QVector<CallHit> calls;
    };

    explicit AsrEngine(QObject* parent = nullptr);
    ~AsrEngine() override;

    bool start(const QString& modelPath, int threads, QString* err = nullptr);
    void stop();
    bool running() const { return running_.load(); }

    // Audio in, any rate-48k block from the SDR's DSP thread. muted = we
    // are transmitting (closes the utterance; our own voice isn't copy).
    void feed48k(const float* x, int n, bool muted);
    // Calls on the band map near the dial (main thread, any time).
    void setHints(const QStringList& calls, qint64 dialHz);
    double queuedSeconds() const;

    // For asrtest: one utterance through whisper, synchronously.
    Result transcribe(const std::vector<float>& pcm16k, const QStringList& hints);

    static QString defaultModelPath();

signals:
    void transcribed(const ttc::AsrEngine::Result& r);
    void status(const QString& s);

private:
    void worker();
    void closeUtterance();
    void cutUtterance();
    static bool promptEcho(const QString& t, const QStringList& hints);
public:
    static QString collapseRepeats(const QString& t);   // public for asrtest
private:
    static bool hallucination(const QString& t);

    whisper_context* ctx_ = nullptr;
    int threads_ = 8;
    std::thread th_;
    std::atomic<bool> running_{false}, stop_{false};

    // DSP-thread side.
    std::vector<float> taps_, hist_;
    int phase_ = 0;
    std::vector<float> frame_;                     // 160 samples @ 16 kHz
    std::vector<float> utt_, preroll_;
    std::vector<float> uttDb_;                     // per-frame level of utt_
    std::deque<float> floorHist_;
    bool inSpeech_ = false;
    int voicedFrames_ = 0, quietFrames_ = 0, loudRun_ = 0;
    float floorDb_ = -60.f;
    qint64 uttStartMs_ = 0;

    mutable std::mutex mux_;
    std::condition_variable cv_;
    struct Utt { std::vector<float> pcm; qint64 atMs; qint64 dialHz; };
    std::deque<Utt> queue_;
    QStringList hints_;
    QHash<QString, qint64> heardCalls_;            // worker thread only
    qint64 dialHz_ = 0;
};

} // namespace ttc

Q_DECLARE_METATYPE(ttc::AsrEngine::Result)
