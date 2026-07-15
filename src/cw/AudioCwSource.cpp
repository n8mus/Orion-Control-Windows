// SPDX-License-Identifier: GPL-2.0-or-later
#include "cw/AudioCwSource.h"
#include "cw/CwDecoder.h"

#include <QProcess>
#include <QSettings>

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
    proc_->start("parec",
                 {"--device=" + dev, "--format=s16le", "--rate=48000",
                  "--channels=1", "--latency-msec=60"});
    emit statusChanged("RADIO audio: " + dev.section('.', 0, 0)
                       + " @ 48 kHz");
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
    buf_.resize(size_t(n));
    constexpr float kNorm = 1.0f / 32768.0f;
    for (int i = 0; i < n; ++i)
        buf_[size_t(i)] = {s[i] * kNorm, 0.0f};   // real audio as complex
    sink_->processIq(buf_.data(), buf_.size());
}

} // namespace ttc
