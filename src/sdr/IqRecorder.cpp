// SPDX-License-Identifier: GPL-2.0-or-later
#include "sdr/IqRecorder.h"

#include <QDateTime>
#include <QDir>
#include <QStandardPaths>
#include <QTimer>
#include <algorithm>
#include <cstring>

namespace ttc {

IqRecorder::IqRecorder(QObject* parent) : QObject(parent) {
    timer_ = new QTimer(this);
    timer_->setInterval(200);
    connect(timer_, &QTimer::timeout, this, &IqRecorder::drain);
}

QString IqRecorder::defaultDir() {
    const QString d =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/iq");
    QDir().mkpath(d);
    return d;
}

bool IqRecorder::start(const QString& path, double sampleRate,
                       double centerHz) {
    if (active_) return true;
    file_.setFileName(path);
    if (!file_.open(QIODevice::WriteOnly)) return false;
    char hdr[32] = {};
    std::memcpy(hdr, "TTCIQ01", 8);
    std::memcpy(hdr + 8, &sampleRate, 8);
    std::memcpy(hdr + 16, &centerHz, 8);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    std::memcpy(hdr + 24, &now, 8);
    file_.write(hdr, sizeof hdr);
    rate_ = sampleRate;
    written_ = 0;
    {
        QMutexLocker lk(&mux_);
        pending_.clear();
        pending_.reserve(1 << 20);
        active_ = true;                    // gate inside the lock: feed()
    }                                      // never sees a half-open file
    timer_->start();
    return true;
}

void IqRecorder::stop() {
    if (!active_) return;
    {
        QMutexLocker lk(&mux_);
        active_ = false;
    }
    timer_->stop();
    drain();
    file_.close();
}

void IqRecorder::feed(const IqBlock& iq) {
    QMutexLocker lk(&mux_);
    if (!active_) return;
    const size_t base = pending_.size();
    pending_.resize(base + iq.size() * 2);
    int16_t* out = pending_.data() + base;
    for (const auto& z : iq) {
        *out++ = int16_t(std::clamp(z.real(), -1.0f, 1.0f) * 32767.0f);
        *out++ = int16_t(std::clamp(z.imag(), -1.0f, 1.0f) * 32767.0f);
    }
}

void IqRecorder::drain() {
    std::vector<int16_t> chunk;
    {
        QMutexLocker lk(&mux_);
        chunk.swap(pending_);
    }
    if (chunk.empty() || !file_.isOpen()) return;
    file_.write(reinterpret_cast<const char*>(chunk.data()),
                qint64(chunk.size() * sizeof(int16_t)));
    written_ += qint64(chunk.size() * sizeof(int16_t));
    emit progress(written_, written_ / (rate_ * 4.0));
}

} // namespace ttc
