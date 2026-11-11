// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/LiveVoice.h"

#include <QProcess>
#include <QTimer>
#ifdef HAVE_RADE
#include "sdr/RadeTx.h"
#endif
#include <algorithm>
#include <cmath>

#ifdef Q_OS_LINUX
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif
#if defined(Q_OS_WIN) && defined(HAVE_QTMULTIMEDIA)
#include "audio/AudioIo.h"
#define TTC_LIVE_QTMM 1
#endif

namespace ttc {

namespace {
constexpr int kRate = 48000;
constexpr int kBlock = 480;                        // 10 ms through the chain
// Backlog control. The mic (Alesis) and the SignaLink run on separate
// clocks and pacat starts after parec, so capture outruns playback and
// the queue only ever grows — live-found: 640 ms sat in the pipe alone
// (a full 64 KB) plus more in our write buffer, 2-3 s mouth-to-radio, so
// the operator unkeyed before his words arrived. Hold it near kTarget by
// dropping 10 ms blocks, in a quiet moment when we can (inaudible), or
// at once past kHard.
constexpr qint64 kBytesPerMs = 96;                 // 48 kHz s16 mono
constexpr qint64 kTargetBytes = 40 * kBytesPerMs;
constexpr qint64 kHardBytes = 120 * kBytesPerMs;
constexpr float kQuiet = 0.01f;                    // -40 dBFS block peak
// RADE: the modem comes in 120 ms bursts and must never lose a sample
// (OFDM), so hold ~130-360 ms queued: prime with silence when it runs low,
// and absorb the mic-vs-SignaLink clock drift by dropping MIC blocks
// (before the encoder — a 10 ms speech gap every few seconds) when high.
constexpr qint64 kRadeLowBytes = 40 * kBytesPerMs;
constexpr qint64 kRadePrimeBytes = 130 * kBytesPerMs;
constexpr qint64 kRadeHighBytes = 360 * kBytesPerMs;
constexpr qint64 kRadeDrainedBytes = 10 * kBytesPerMs;
constexpr int kRadeDeviceTailMs = 150;             // pacat + SignaLink latency
}

LiveVoice::LiveVoice(QObject* parent) : QObject(parent) {}

LiveVoice::~LiveVoice() {
    stop();
#ifdef HAVE_RADE
    delete radeTx_;
#endif
}

bool LiveVoice::setRade(bool on, const QString& callsign) {
#ifdef HAVE_RADE
    if (on && !radeTx_) radeTx_ = new RadeTx(callsign.toStdString());
    else if (on) radeTx_->setCallsign(callsign.toStdString());
    if (!on) { delete radeTx_; radeTx_ = nullptr; }
    radeEnding_ = radeDrainArmed_ = false;
    return on == (radeTx_ != nullptr);
#else
    (void)callsign;
    return !on;
#endif
}

void LiveVoice::setRadeLevelDb(float db) {
#ifdef HAVE_RADE
    if (radeTx_) radeTx_->setLevelDb(db);
#else
    (void)db;
#endif
}

void LiveVoice::endOverRade() {
#ifdef HAVE_RADE
    if (!radeTx_ || !haveOut() || radeEnding_) { emit radeOverSent(); return; }
    std::vector<float> m;
    radeTx_->endOver(m);
    QByteArray out;
    out.reserve(qsizetype(m.size()) * 2);
    for (float v : m) {
        const qint16 q = qint16(std::clamp(int(std::lround((muted_ ? 0.f : v) * 32767.f)), -32768, 32767));
        out.append(reinterpret_cast<const char*>(&q), 2);
    }
    writeOut(out);
    radeEnding_ = true;
    radeDrainArmed_ = false;
#else
    emit radeOverSent();
#endif
}

bool LiveVoice::running() const {
#ifdef TTC_LIVE_QTMM
    return cap_ && cap_->running() && sink_ && sink_->running();
#else
    return rec_ && rec_->state() != QProcess::NotRunning
        && play_ && play_->state() != QProcess::NotRunning;
#endif
}

bool LiveVoice::haveOut() const {
#ifdef TTC_LIVE_QTMM
    return sink_ != nullptr;
#else
    return play_ != nullptr;
#endif
}

void LiveVoice::writeOut(const QByteArray& pcm) {
#ifdef TTC_LIVE_QTMM
    if (sink_) sink_->write(pcm);
#else
    if (play_) play_->write(pcm);
#endif
}

bool LiveVoice::start(const QString& sourceNode, const QString& sinkNode,
                      const VoiceEqSettings& s) {
    stop();
    stopping_ = false;
    carry_.clear();
    chain_.prepare(kRate, s, s.comp.makeupDb);
#ifdef TTC_LIVE_QTMM
    // The same two streams as below, on the Windows endpoints: the nodes
    // are device descriptions here (ClipDeck::findSink / findSource).
    QString why;
    sink_ = new PcmSink(this);
    const auto note = connect(sink_, &PcmSink::errorText, this,
                              [&why](const QString& w) { why = w; });
    const bool outOk = sink_->start(sinkNode, kRate, 30);
    disconnect(note);
    cap_ = new AudioCapture(this);
    connect(cap_, &AudioCapture::chunk, this, [this](const QByteArray& pcm) {
        carry_ += pcm;
        onCaptured();
    });
    const auto note2 = connect(cap_, &AudioCapture::errorText, this,
                               [&why](const QString& w) { why = w; });
    const bool inOk = outOk && cap_->start(sourceNode, kRate);
    disconnect(note2);
    if (!outOk || !inOk) {
        stop();
        emit failed(why.isEmpty() ? QStringLiteral("the mic or the radio's sound device would not open")
                                  : why);
        return false;
    }
    dropped_ = 0;
    return true;
#else
    play_ = new QProcess(this);
    play_->start("pacat", {"--playback", "--raw", "--format=s16le",
                           "--rate=48000", "--channels=1",
                           "--latency-msec=30", "--device=" + sinkNode});
    rec_ = new QProcess(this);
    connect(rec_, &QProcess::readyReadStandardOutput, this, &LiveVoice::onCaptured);
    const auto died = [this](const char* who) {
        return [this, who](int code, QProcess::ExitStatus) {
            if (stopping_) return;
            emit failed(QString("%1 ended (%2)").arg(who).arg(code));
        };
    };
    connect(rec_, &QProcess::finished, this, died("mic capture"));
    connect(play_, &QProcess::finished, this, died("SignaLink playback"));
    rec_->start("parec", {"--raw", "--format=s16le", "--rate=48000",
                          "--channels=1", "--latency-msec=20",
                          "--device=" + sourceNode});
    if (!play_->waitForStarted(2000) || !rec_->waitForStarted(2000)) {
        stop();
        emit failed("parec/pacat would not start");
        return false;
    }
    dropped_ = 0;
#ifdef Q_OS_LINUX
    // Open pacat's stdin pipe a second time, read end, purely to measure
    // what is queued in it (FIONREAD) — never read from it — and shrink it
    // from 64 KB (680 ms!) to one page (~40 ms).
    probeFd_ = ::open(QString("/proc/%1/fd/0").arg(play_->processId())
                          .toLocal8Bit().constData(), O_RDONLY | O_NONBLOCK);
    if (probeFd_ >= 0) ::fcntl(probeFd_, F_SETPIPE_SZ, 4096);
#endif
    return true;
#endif  // TTC_LIVE_QTMM
}

void LiveVoice::stop() {
    stopping_ = true;
#ifdef Q_OS_LINUX
    if (probeFd_ >= 0) ::close(probeFd_);
#endif
    probeFd_ = -1;
#ifdef TTC_LIVE_QTMM
    if (cap_) { cap_->disconnect(this); cap_->stop(); cap_->deleteLater(); cap_ = nullptr; }
    if (sink_) { sink_->disconnect(this); sink_->stop(); sink_->deleteLater(); sink_ = nullptr; }
#endif
    for (QProcess** p : {&rec_, &play_}) {
        if (!*p) continue;
        (*p)->disconnect(this);
        (*p)->kill();
        (*p)->waitForFinished(500);
        (*p)->deleteLater();
        *p = nullptr;
    }
    carry_.clear();
}

qint64 LiveVoice::backlogBytes() const {
#ifdef TTC_LIVE_QTMM
    return sink_ ? sink_->queuedBytes() : 0;
#endif
    qint64 n = play_ ? play_->bytesToWrite() : 0;
#ifdef Q_OS_LINUX
    int inPipe = 0;
    if (probeFd_ >= 0 && ::ioctl(probeFd_, FIONREAD, &inPipe) == 0) n += inPipe;
#endif
    return n;
}

double LiveVoice::backlogMs() const {
    return double(backlogBytes()) / double(kBytesPerMs);
}

void LiveVoice::setSettings(const VoiceEqSettings& s) {
    chain_.configure(s, s.comp.makeupDb);
}

void LiveVoice::onCaptured() {
#ifdef TTC_LIVE_QTMM
    if (!cap_ || !sink_) return;               // the chunk is already in carry_
#else
    if (!rec_ || !play_) return;
    carry_ += rec_->readAllStandardOutput();
#endif
    const int frames = int(carry_.size() / 2);
    if (frames < kBlock) return;
    const int take = frames / kBlock * kBlock;     // whole blocks only
    buf_.resize(size_t(take));
    const auto* in = reinterpret_cast<const qint16*>(carry_.constData());
    float pk = 0.f;
    for (int i = 0; i < take; ++i) {
        buf_[size_t(i)] = in[i] / 32768.0f;
        pk = std::max(pk, std::fabs(buf_[size_t(i)]));
    }
    carry_.remove(0, qsizetype(take) * 2);
    inPeakDb_ = 20.f * std::log10(std::max(pk, 1e-6f));
    if (radeTx_) { onCapturedRade(take); return; }
    for (int o = 0; o < take; o += kBlock) chain_.process(buf_.data() + o, kBlock);
    // Write block by block, dropping whole 10 ms blocks while the queue is
    // over target — quiet ones only, unless it is far over.
    float opk = 0.f;
    for (int i = 0; i < take; ++i) opk = std::max(opk, std::fabs(buf_[size_t(i)]));
    outPeakDb_ = muted_ ? -120.f : 20.f * std::log10(std::max(opk, 1e-6f));
    QByteArray out;
    out.reserve(qsizetype(take) * 2);
    qint64 queued = backlogBytes();
    for (int o = 0; o < take; o += kBlock) {
        float pk = 0.f;
        for (int i = 0; i < kBlock; ++i)
            pk = std::max(pk, std::fabs(buf_[size_t(o + i)]));
        const bool over = queued > kTargetBytes;
        if (queued > kHardBytes || (over && pk < kQuiet)) {
            ++dropped_;
            queued -= kBlock * 2;                  // what writing would have added
            continue;
        }
        for (int i = 0; i < kBlock; ++i) {
            const float v = muted_ ? 0.f : buf_[size_t(o + i)];
            const qint16 q = qint16(std::clamp(int(std::lround(v * 32767.f)), -32768, 32767));
            out.append(reinterpret_cast<const char*>(&q), 2);
        }
        queued += kBlock * 2;
    }
    if (!out.isEmpty()) writeOut(out);
}

void LiveVoice::onCapturedRade(int take) {
#ifdef HAVE_RADE
    const qint64 queued = backlogBytes();
    if (radeEnding_) {
        // The end of over is in the queue: send nothing more (the mic is
        // not encoded), and once it has played out into the SignaLink —
        // plus the device's own latency — PTT may drop.
        if (queued <= kRadeDrainedBytes && !radeDrainArmed_) {
            radeDrainArmed_ = true;
            QTimer::singleShot(kRadeDeviceTailMs, this, [this] {
                if (!radeEnding_) return;
                radeEnding_ = radeDrainArmed_ = false;
                emit radeOverSent();
            });
        }
        outPeakDb_ = -120.f;
        return;
    }
    QByteArray out;
    if (queued < kRadeLowBytes)                    // starved: prime with silence
        out.fill('\0', qsizetype(kRadePrimeBytes - queued) & ~qsizetype(1));
    std::vector<float> m;
    if (queued > kRadeHighBytes) {
        ++dropped_;                                // drift: skip this mic chunk
    } else {
        radeTx_->process(buf_.data(), take, m);
    }
    float opk = 0.f;
    for (float v : m) opk = std::max(opk, std::fabs(v));
    if (!m.empty()) outPeakDb_ = muted_ ? -120.f : 20.f * std::log10(std::max(opk, 1e-6f));
    out.reserve(out.size() + qsizetype(m.size()) * 2);
    for (float v : m) {
        const qint16 q = qint16(std::clamp(int(std::lround((muted_ ? 0.f : v) * 32767.f)), -32768, 32767));
        out.append(reinterpret_cast<const char*>(&q), 2);
    }
    if (!out.isEmpty()) writeOut(out);
#else
    (void)take;
#endif
}

} // namespace ttc
