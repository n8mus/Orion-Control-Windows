// SPDX-License-Identifier: GPL-2.0-or-later
#include "audio/AudioIo.h"

#include <QAudio>
#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QAudioSource>
#include <QIODevice>
#include <QMediaDevices>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ttc {

// ---------------------------------------------------------------- resampler

void PcmResampler::reset(double srcRate, double dstRate) {
    step_ = srcRate / dstRate;
    made_ = 0;
    dropped_ = 0;
    buf_.clear();
    primed_ = false;
}

void PcmResampler::process(const int16_t* in, size_t n,
                           std::vector<int16_t>& out) {
    if (n == 0) return;
    if (step_ == 1.0) {                    // rates match: pass through
        out.insert(out.end(), in, in + n);
        return;
    }
    // buf_ is the input not finished with yet. Index 0 is one sample BEFORE
    // the stream's start, a copy of the first sample, so that the first
    // real sample (index 1, where pos_ begins) has a left neighbour.
    if (!primed_) {
        buf_.push_back(float(in[0]));
        primed_ = true;
    }
    buf_.reserve(buf_.size() + n);
    for (size_t k = 0; k < n; ++k) buf_.push_back(float(in[k]));

    // One output per step while the four points around the read position
    // are all here: i-1, i, i+1, i+2 with i = floor(pos) >= 1. What is
    // missing at the end of this chunk arrives with the next one. pos is
    // in buf_'s own index space: the stream position (+1 for the copy at
    // index 0) less what has been dropped from the front.
    const size_t size = buf_.size();
    double pos = 0.0;
    while (true) {
        pos = 1.0 + double(made_) * step_ - double(dropped_);
        const size_t i = size_t(pos);
        if (i + 2 >= size) break;
        const double t = pos - double(i);
        const double p0 = buf_[i - 1], p1 = buf_[i], p2 = buf_[i + 1], p3 = buf_[i + 2];
        // Catmull-Rom: through p1 (t=0) and p2 (t=1), slopes from the
        // neighbours on either side.
        const double v = p1 + 0.5 * t * ((p2 - p0)
                       + t * ((2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3)
                       + t * (3.0 * (p1 - p2) + p3 - p0)));
        out.push_back(int16_t(std::clamp(std::lround(v), -32768L, 32767L)));
        ++made_;
    }
    // Drop what no later output can need: everything before floor(pos)-1.
    const size_t want = size_t(pos) - 1;
    const size_t drop = std::min(want, buf_.size());
    if (drop > 0) {
        buf_.erase(buf_.begin(), buf_.begin() + std::ptrdiff_t(drop));
        dropped_ += drop;
    }
}

// ------------------------------------------------------------ device match

static QAudioDevice matchInput(const QString& needle) {
    const auto devs = QMediaDevices::audioInputs();
    if (!needle.trimmed().isEmpty())
        for (const QAudioDevice& d : devs)
            if (d.description().contains(needle.trimmed(), Qt::CaseInsensitive))
                return d;
    return QMediaDevices::defaultAudioInput();
}

static QAudioDevice matchOutput(const QString& needle) {
    const auto devs = QMediaDevices::audioOutputs();
    if (!needle.trimmed().isEmpty())
        for (const QAudioDevice& d : devs)
            if (d.description().contains(needle.trimmed(), Qt::CaseInsensitive))
                return d;
    return QMediaDevices::defaultAudioOutput();
}

// Pick the closest format the device supports, preferring the exact ask
// (rate, mono, Int16); conversion covers whatever remains. Returns false
// only when the device supports nothing usable.
static bool pickFormat(const QAudioDevice& dev, int wantRate,
                       QAudioFormat& out) {
    QAudioFormat f;
    f.setSampleRate(wantRate);
    f.setChannelCount(1);
    f.setSampleFormat(QAudioFormat::Int16);
    if (dev.isFormatSupported(f)) { out = f; return true; }
    // Device's own preference, nudged toward Int16/mono where allowed.
    QAudioFormat p = dev.preferredFormat();
    QAudioFormat t = p;
    t.setSampleFormat(QAudioFormat::Int16);
    t.setChannelCount(1);
    if (dev.isFormatSupported(t)) { out = t; return true; }
    t = p;
    t.setSampleFormat(QAudioFormat::Int16);
    if (dev.isFormatSupported(t)) { out = t; return true; }
    if (p.isValid()) { out = p; return true; }
    return false;
}

// Convert one device-format buffer to s16 mono (no resampling here).
static void toS16Mono(const char* data, qsizetype bytes, const QAudioFormat& f,
                      std::vector<int16_t>& out) {
    const int ch = f.channelCount();
    switch (f.sampleFormat()) {
        case QAudioFormat::Int16: {
            const auto* s = reinterpret_cast<const int16_t*>(data);
            const qsizetype frames = bytes / (2 * ch);
            for (qsizetype i = 0; i < frames; ++i) {
                int acc = 0;
                for (int c = 0; c < ch; ++c) acc += s[i * ch + c];
                out.push_back(int16_t(acc / ch));
            }
            break;
        }
        case QAudioFormat::Float: {
            const auto* s = reinterpret_cast<const float*>(data);
            const qsizetype frames = bytes / (4 * ch);
            for (qsizetype i = 0; i < frames; ++i) {
                float acc = 0.0f;
                for (int c = 0; c < ch; ++c) acc += s[i * ch + c];
                const float v = std::clamp(acc / ch, -1.0f, 1.0f);
                out.push_back(int16_t(v * 32767.0f));
            }
            break;
        }
        case QAudioFormat::Int32: {
            const auto* s = reinterpret_cast<const int32_t*>(data);
            const qsizetype frames = bytes / (4 * ch);
            for (qsizetype i = 0; i < frames; ++i) {
                qint64 acc = 0;
                for (int c = 0; c < ch; ++c) acc += s[i * ch + c];
                out.push_back(int16_t((acc / ch) >> 16));
            }
            break;
        }
        case QAudioFormat::UInt8: {
            const auto* s = reinterpret_cast<const uint8_t*>(data);
            const qsizetype frames = bytes / (1 * ch);
            for (qsizetype i = 0; i < frames; ++i) {
                int acc = 0;
                for (int c = 0; c < ch; ++c) acc += (int(s[i * ch + c]) - 128);
                out.push_back(int16_t((acc / ch) * 256));
            }
            break;
        }
        default: break;
    }
}

// ------------------------------------------------------------------ capture

AudioCapture::AudioCapture(QObject* parent) : QObject(parent) {}

AudioCapture::~AudioCapture() { stop(); }

QStringList AudioCapture::inputDescriptions() {
    QStringList out;
    for (const QAudioDevice& d : QMediaDevices::audioInputs())
        out << d.description();
    return out;
}

bool AudioCapture::start(const QString& deviceMatch, int rateHz) {
    stop();
    const QAudioDevice dev = matchInput(deviceMatch);
    if (dev.isNull()) {
        emit errorText(QStringLiteral("no audio input device"));
        return false;
    }
    QAudioFormat fmt;
    if (!pickFormat(dev, rateHz, fmt)) {
        emit errorText(QStringLiteral("%1: no usable capture format")
                           .arg(dev.description()));
        return false;
    }
    dstRate_ = rateHz;
    srcRate_ = fmt.sampleRate();
    srcChannels_ = fmt.channelCount();
    srcSampleFmt_ = int(fmt.sampleFormat());
    resamp_.reset(srcRate_, dstRate_);
    carry_.clear();
    desc_ = dev.description();

    src_ = new QAudioSource(dev, fmt, this);
    // Small buffer: this feeds a live decoder/meter. ~80 ms at the device
    // rate keeps latency low without starving on scheduler hiccups.
    src_->setBufferSize(fmt.bytesForDuration(80000));
    io_ = src_->start();
    if (!io_) {
        emit errorText(QStringLiteral("%1: capture failed to start")
                           .arg(dev.description()));
        delete src_;
        src_ = nullptr;
        desc_.clear();
        return false;
    }
    connect(io_, &QIODevice::readyRead, this, &AudioCapture::onReadable);
    return true;
}

void AudioCapture::stop() {
    if (io_) { io_->disconnect(this); io_ = nullptr; }
    if (src_) { src_->stop(); src_->deleteLater(); src_ = nullptr; }
    desc_.clear();
    carry_.clear();
}

void AudioCapture::onReadable() {
    if (!io_) return;
    QByteArray data = carry_ + io_->readAll();
    QAudioFormat f;
    f.setSampleRate(srcRate_);
    f.setChannelCount(srcChannels_);
    f.setSampleFormat(QAudioFormat::SampleFormat(srcSampleFmt_));
    const int frameBytes = f.bytesPerFrame() > 0 ? f.bytesPerFrame() : 2;
    const qsizetype usable = data.size() - data.size() % frameBytes;
    carry_ = data.mid(usable);
    if (usable <= 0) return;
    monoBuf_.clear();
    toS16Mono(data.constData(), usable, f, monoBuf_);
    if (monoBuf_.empty()) return;
    outBuf_.clear();
    resamp_.process(monoBuf_.data(), monoBuf_.size(), outBuf_);
    if (outBuf_.empty()) return;
    emit chunk(QByteArray(reinterpret_cast<const char*>(outBuf_.data()),
                          qsizetype(outBuf_.size() * 2)));
}

// ----------------------------------------------------------------- playback

AudioPlayback::AudioPlayback(QObject* parent) : QObject(parent) {}

AudioPlayback::~AudioPlayback() { stop(); }

QStringList AudioPlayback::outputDescriptions() {
    QStringList out;
    for (const QAudioDevice& d : QMediaDevices::audioOutputs())
        out << d.description();
    return out;
}

bool AudioPlayback::start(const QString& deviceMatch, int rateHz, int bufferMs,
                          bool nativeFormat) {
    stop();
    const QAudioDevice dev = matchOutput(deviceMatch);
    if (dev.isNull()) {
        emit errorText(QStringLiteral("no audio output device"));
        return false;
    }
    QAudioFormat fmt;
    bool haveFmt = false;
    if (nativeFormat) {
        // The device's own format, when it is one write() can fill (Int16
        // or Float, any channel count, any rate).
        const QAudioFormat own = dev.preferredFormat();
        if (own.isValid()
            && (own.sampleFormat() == QAudioFormat::Int16
                || own.sampleFormat() == QAudioFormat::Float)) {
            fmt = own;
            haveFmt = true;
        }
    }
    if (!haveFmt && !pickFormat(dev, rateHz, fmt)) {
        emit errorText(QStringLiteral("%1: no usable playback format")
                           .arg(dev.description()));
        return false;
    }
    srcRate_ = rateHz;
    dstRate_ = fmt.sampleRate();
    dstChannels_ = fmt.channelCount();
    dstSampleFmt_ = int(fmt.sampleFormat());
    resamp_.reset(srcRate_, dstRate_);
    carry_.clear();

    sink_ = new QAudioSink(dev, fmt, this);
    // Generous buffer by default: playback rides network/capture jitter
    // (RIP packets, DVR reads); ~300 ms absorbs it, and latency there is
    // not operator-facing. Live streams pass their own, smaller figure.
    sink_->setBufferSize(
        fmt.bytesForDuration(qint64(std::clamp(bufferMs, 10, 2000)) * 1000));
    io_ = sink_->start();
    if (!io_) {
        emit errorText(QStringLiteral("%1: playback failed to start")
                           .arg(dev.description()));
        delete sink_;
        sink_ = nullptr;
        return false;
    }
    return true;
}

void AudioPlayback::stop() {
    io_ = nullptr;                         // owned by the sink
    if (sink_) { sink_->stop(); sink_->deleteLater(); sink_ = nullptr; }
    carry_.clear();
}

qint64 AudioPlayback::bytesFree() const {
    if (!sink_) return 0;
    // Device bytes → caller bytes: unwind channel fan-out, sample width,
    // and the resample ratio. Conservative floor of one frame.
    const int devBytesPerSample =
        dstSampleFmt_ == int(QAudioFormat::Float) ? 4 : 2;
    const double devToSrc = double(srcRate_) / double(dstRate_);
    const qint64 devFrames =
        sink_->bytesFree() / (devBytesPerSample * dstChannels_);
    return qint64(double(devFrames) * devToSrc) * 2;
}

qint64 AudioPlayback::bytesQueued() const {
    if (!sink_) return 0;
    const int devBytesPerSample =
        dstSampleFmt_ == int(QAudioFormat::Float) ? 4 : 2;
    const double devToSrc = double(srcRate_) / double(dstRate_);
    const qint64 queued =
        std::max<qint64>(0, sink_->bufferSize() - sink_->bytesFree());
    const qint64 devFrames = queued / (devBytesPerSample * dstChannels_);
    return qint64(double(devFrames) * devToSrc) * 2;
}

bool AudioPlayback::drained() const {
    return !sink_ || sink_->state() == QAudio::IdleState
                  || sink_->state() == QAudio::StoppedState;
}

void AudioPlayback::write(const QByteArray& s16leMono) {
    if (!io_) return;
    QByteArray data = carry_ + s16leMono;
    const qsizetype usable = data.size() & ~1;
    carry_ = data.mid(usable);
    const auto* in = reinterpret_cast<const int16_t*>(data.constData());
    const size_t n = size_t(usable / 2);
    if (n == 0) return;
    outBuf_.clear();
    static thread_local std::vector<int16_t> resampled;
    resampled.clear();
    resamp_.process(in, n, resampled);
    if (resampled.empty()) return;
    // Fan mono out to the device's channel count / sample format.
    if (dstSampleFmt_ == int(QAudioFormat::Int16) && dstChannels_ == 1) {
        io_->write(reinterpret_cast<const char*>(resampled.data()),
                   qsizetype(resampled.size() * 2));
        return;
    }
    QByteArray out;
    if (dstSampleFmt_ == int(QAudioFormat::Int16)) {
        out.resize(qsizetype(resampled.size() * 2 * dstChannels_));
        auto* o = reinterpret_cast<int16_t*>(out.data());
        for (size_t i = 0; i < resampled.size(); ++i)
            for (int c = 0; c < dstChannels_; ++c)
                o[i * dstChannels_ + c] = resampled[i];
    } else if (dstSampleFmt_ == int(QAudioFormat::Float)) {
        out.resize(qsizetype(resampled.size() * 4 * dstChannels_));
        auto* o = reinterpret_cast<float*>(out.data());
        for (size_t i = 0; i < resampled.size(); ++i) {
            const float v = resampled[i] / 32768.0f;
            for (int c = 0; c < dstChannels_; ++c)
                o[i * dstChannels_ + c] = v;
        }
    } else {
        return;                            // exotic sink format: stay silent
    }
    io_->write(out);
}

// ------------------------------------------------------------------ PcmSink

PcmSink::PcmSink(QObject* parent) : QObject(parent), ctx_(new QObject) {}

PcmSink::~PcmSink() {
    stop();
    if (thread_) {
        ctx_->deleteLater();               // runs as the thread winds down
        thread_->quit();
        thread_->wait();
        delete thread_;
    } else {
        delete ctx_;
    }
}

void PcmSink::useOwnThread() {
    if (thread_ || running_.load()) return;
    thread_ = new QThread;
    thread_->setObjectName(QStringLiteral("ttc-pcm-sink"));
    ctx_->moveToThread(thread_);
    // No COM set-up here: Qt Multimedia's Windows backend initializes COM
    // itself on whichever thread makes the sink, and doing it first in a
    // different mode only earns "Cannot change thread mode after it is set".
    thread_->start(QThread::TimeCriticalPriority);
}

template <typename F> void PcmSink::onDeviceThread(F&& f) {
    if (thread_ && QThread::currentThread() != thread_)
        QMetaObject::invokeMethod(ctx_, std::forward<F>(f),
                                  Qt::BlockingQueuedConnection);
    else
        f();
}

bool PcmSink::start(const QString& deviceMatch, int rateHz, int deviceBufferMs,
                    int queueCapMs) {
    stop();
    {
        QMutexLocker lk(&mux_);
        fifo_.clear();
        capBytes_ = qint64(rateHz) * 2 * std::max(queueCapMs, 100) / 1000;
    }
    minChunk_ = (qint64(rateHz) * 2 * 5 / 1000) & ~qint64(1);   // 5 ms
    fifoBytes_.store(0);
    devBytes_.store(0);
    bool ok = false;
    onDeviceThread([&] {
        pb_ = new AudioPlayback(ctx_);
        connect(pb_, &AudioPlayback::errorText, this, &PcmSink::errorText);
        ok = pb_->start(deviceMatch, rateHz, deviceBufferMs, true);
        if (!ok) {
            delete pb_;
            pb_ = nullptr;
            return;
        }
        timer_ = new QTimer(ctx_);
        timer_->setTimerType(Qt::PreciseTimer);
        timer_->setInterval(5);
        connect(timer_, &QTimer::timeout, ctx_, [this] { pump(); });
        timer_->start();
    });
    running_.store(ok);
    return ok;
}

void PcmSink::stop() {
    const bool was = running_.exchange(false);
    if (!was && !pb_) return;
    onDeviceThread([this] {
        delete timer_;
        timer_ = nullptr;
        if (pb_) {
            pb_->stop();
            delete pb_;
            pb_ = nullptr;
        }
    });
    QMutexLocker lk(&mux_);
    fifo_.clear();
    fifoBytes_.store(0);
    devBytes_.store(0);
}

void PcmSink::write(const char* s16leMono, qint64 bytes) {
    if (bytes <= 0 || !running_.load()) return;
    QMutexLocker lk(&mux_);
    if (fifo_.size() + bytes > capBytes_) return;      // device stalled: drop
    fifo_.append(s16leMono, qsizetype(bytes));
    fifoBytes_.store(fifo_.size());
}

qint64 PcmSink::backlogBytes() const {
    return fifoBytes_.load() + devBytes_.load();
}

void PcmSink::pump() {
    if (!pb_) return;
    const qint64 room = pb_->bytesFree() & ~qint64(1);
    QByteArray chunk;
    if (room >= minChunk_) {                           // never slivers
        QMutexLocker lk(&mux_);
        const qsizetype take =
            qsizetype(std::min<qint64>(room, fifo_.size())) & ~qsizetype(1);
        if (take > 0) {
            chunk = fifo_.left(take);
            fifo_.remove(0, take);
            fifoBytes_.store(fifo_.size());
        }
    }
    if (!chunk.isEmpty()) pb_->write(chunk);
    devBytes_.store(pb_->bytesQueued());
}

} // namespace ttc
