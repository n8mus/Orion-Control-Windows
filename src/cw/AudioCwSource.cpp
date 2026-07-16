// SPDX-License-Identifier: GPL-2.0-or-later
#include "cw/AudioCwSource.h"
#include "cw/CwDecoder.h"

#include <QProcess>
#include <QSettings>

#include <rnnoise.h>

namespace ttc {

namespace {
// The SignaLink's PipeWire source node. A different sound device can be
// set via cw/audioDev (any substring parec accepts as --device, or a
// full node name from `pactl list sources short`).
const char* kDefaultDev =
    "alsa_input.usb-BurrBrown_from_Texas_Instruments_USB_AUDIO_CODEC-00"
    ".analog-stereo";
} // namespace

AudioCwSource::AudioCwSource(CwDecoder* sink, QObject* parent)
    : QObject(parent), sink_(sink) {}

bool AudioCwSource::running() const {
    return proc_ && proc_->state() != QProcess::NotRunning;
}

void AudioCwSource::start() {
    if (running()) return;
    const QString dev =
        QSettings().value("cw/audioDev", kDefaultDev).toString();
    if (!proc_) {
        proc_ = new QProcess(this);
        connect(proc_, &QProcess::readyReadStandardOutput, this,
                &AudioCwSource::onReadable);
        connect(proc_, &QProcess::errorOccurred, this, [this] {
            emit statusChanged("RADIO audio: parec failed — is "
                               "pulseaudio-utils installed?");
        });
        connect(proc_, &QProcess::finished, this, [this](int code, auto) {
            if (code != 0)
                emit statusChanged(
                    QString("RADIO audio: capture ended (%1) — check the "
                            "device in cw/audioDev").arg(code));
        });
    }
    carry_.clear();
    nrFill_ = 0;
    if (nrSt_) { rnnoise_destroy(nrSt_); nrSt_ = nullptr; }
    proc_->start("parec",
                 {"--device=" + dev, "--format=s16le", "--rate=48000",
                  "--channels=1", "--latency-msec=60"});
    emit statusChanged("RADIO audio: " + dev.section('.', 0, 0)
                       + " @ 48 kHz");
}

void AudioCwSource::setNr(bool on) {
    nrOn_ = on;
    nrFill_ = 0;
}

void AudioCwSource::stop() {
    if (!running()) return;
    proc_->kill();
    proc_->waitForFinished(500);
    carry_.clear();
}

void AudioCwSource::onReadable() {
    QByteArray data = carry_ + proc_->readAllStandardOutput();
    const int usable = data.size() & ~1;   // whole int16 samples only
    carry_ = data.mid(usable);
    const int n = usable / 2;
    if (n <= 0 || !sink_) return;
    const auto* s = reinterpret_cast<const int16_t*>(data.constData());
    constexpr float kNorm = 1.0f / 32768.0f;
    if (!nrOn_) {
        buf_.resize(size_t(n));
        for (int i = 0; i < n; ++i)
            buf_[size_t(i)] = {s[i] * kNorm, 0.0f};
        sink_->processIq(buf_.data(), buf_.size());
        return;
    }
    // RNNoise path: 480-sample frames, s16-range floats in and out.
    if (!nrSt_) nrSt_ = rnnoise_create(nullptr);
    const int fs = rnnoise_get_frame_size();
    if (int(nrIn_.size()) != fs) nrIn_.assign(size_t(fs), 0.0f);
    buf_.clear();
    static thread_local std::vector<float> fout;
    fout.resize(size_t(fs));
    for (int i = 0; i < n; ++i) {
        nrIn_[size_t(nrFill_)] = float(s[i]);
        if (++nrFill_ < fs) continue;
        nrFill_ = 0;
        rnnoise_process_frame(nrSt_, fout.data(), nrIn_.data());
        for (int k = 0; k < fs; ++k)
            buf_.push_back({fout[size_t(k)] * kNorm, 0.0f});
    }
    if (!buf_.empty()) sink_->processIq(buf_.data(), buf_.size());
}

} // namespace ttc
