// SPDX-License-Identifier: GPL-2.0-or-later
#include "audio/AudioIo.h"

#include <QAudio>
#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QAudioSource>
#include <QIODevice>
#include <QMediaDevices>

#include <algorithm>
#include <cstring>

namespace ttc {

// ---------------------------------------------------------------- resampler

void LinearResampler::reset(double srcRate, double dstRate) {
    step_ = srcRate / dstRate;
    pos_ = 0.0;
    last_ = 0;
    primed_ = false;
}

void LinearResampler::process(const int16_t* in, size_t n,
                              std::vector<int16_t>& out) {
    if (n == 0) return;
    if (step_ == 1.0) {                    // rates match: pass through
        out.insert(out.end(), in, in + n);
        return;
    }
    // Virtual source stream: last_ (index -1, once primed) then in[0..n).
    // pos_ is the fractional read position relative to in[0].
    while (true) {
        const double p = pos_;
        const long i = long(p);            // floor for p >= -1
        if (p >= double(n - 1) + 1e-9) break;   // need next chunk
        double a, b;
        if (p < 0.0) {
            if (!primed_) { pos_ = 0.0; continue; }
            a = last_;
            b = in[0];
        } else {
            a = in[i];
            b = (size_t(i + 1) < n) ? in[i + 1] : in[i];
        }
        const double frac = p - double(i < 0 ? -1 : i);
        const double v = a + (b - a) * frac;
        out.push_back(int16_t(std::clamp(v, -32768.0, 32767.0)));
        pos_ += step_;
    }
    pos_ -= double(n);                     // rebase onto the next chunk
    last_ = in[n - 1];
    primed_ = true;
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

bool AudioPlayback::start(const QString& deviceMatch, int rateHz) {
    stop();
    const QAudioDevice dev = matchOutput(deviceMatch);
    if (dev.isNull()) {
        emit errorText(QStringLiteral("no audio output device"));
        return false;
    }
    QAudioFormat fmt;
    if (!pickFormat(dev, rateHz, fmt)) {
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
    // Generous buffer: playback rides network/capture jitter (RIP packets,
    // DVR reads); ~300 ms absorbs it, and latency here is not operator-
    // facing the way the decode path is.
    sink_->setBufferSize(fmt.bytesForDuration(300000));
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

} // namespace ttc
