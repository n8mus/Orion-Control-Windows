// SPDX-License-Identifier: GPL-2.0-or-later
#include "radio/TripAudio.h"

#include <QHostAddress>
#include <QTimer>
#include <QUdpSocket>
#include <cstdio>

namespace {
constexpr int kSampleRate = 7013;
constexpr int kSamplesPerPkt = 128;
}

namespace ttc {

TripAudio::TripAudio(QObject* parent) : QObject(parent) {
    selftest_ = qEnvironmentVariableIsSet("TTC_SELFTEST");
}

TripAudio::~TripAudio() { stop(); }

QByteArray TripAudio::packetize(quint8& counter, const char* s16le) {
    QByteArray p;
    p.reserve(1 + 128);
    p.append(char(counter++));                   // wraps at 256
    for (int i = 0; i < 128; ++i)
        p.append(s16le[2 * i + 1]);              // high byte = signed 8-bit
    return p;
}

bool TripAudio::start(quint32 host, quint16 cmdPort, const QString& source) {
    if (sock_) return true;
    host_ = host;
    audioPort_ = quint16(cmdPort + 4);
    counter_ = 0;
    acc_.clear();
    pkts_ = 0;
    sock_ = new QUdpSocket(this);
    // Symmetric ports, same rule as RIP/command: bind the audio port locally.
    if (!sock_->bind(QHostAddress::AnyIPv4, audioPort_)) {
        delete sock_;
        sock_ = nullptr;
        return false;
    }
    if (selftest_) return true;                  // no capture under the harness
    rec_ = new QProcess(this);
    // --latency small so pw-record hands us frequent little chunks instead of
    // big bursts; the pacer below smooths whatever jitter remains.
    QStringList args{"--raw",         "--format=s16", "--rate=7013",
                     "--channels=1",  "--latency=20ms"};
    const QString src = source.trimmed();
    if (!src.isEmpty()) args << "--target" << src;
    args << "-";
    connect(rec_, &QProcess::readyReadStandardOutput, this,
            &TripAudio::onCapture);
    rec_->start("pw-record", args);
    if (!rec_->waitForStarted(1500)) {
        rec_->deleteLater();
        rec_ = nullptr;                          // no capture: keyed, but silent
    }
    // Pace packets out at the audio rate. The radio's transmit buffer needs a
    // STEADY ~55 pkt/s; dumping each capture burst onto the wire immediately
    // floods then starves it (gated/"dashed" TX audio). onCapture only fills
    // acc_; this timer drains it on a real-time schedule.
    paced_ = 0;
    streaming_ = false;
    clock_.start();
    pacer_ = new QTimer(this);
    pacer_->setInterval(4);
    connect(pacer_, &QTimer::timeout, this, &TripAudio::drain);
    pacer_->start();
    return true;
}

void TripAudio::stop() {
    if (pacer_) {
        pacer_->stop();
        pacer_->deleteLater();
        pacer_ = nullptr;
    }
    if (rec_) {
        rec_->terminate();
        rec_->waitForFinished(500);
        rec_->deleteLater();
        rec_ = nullptr;
    }
    if (sock_) {
        sock_->deleteLater();
        sock_ = nullptr;
    }
    acc_.clear();
    if (selftest_)
        fprintf(stderr, "[trip] %llu audio pkts sent\n",
                static_cast<unsigned long long>(pkts_));
}

void TripAudio::onCapture() {
    if (!rec_) return;
    acc_ += rec_->readAllStandardOutput();       // buffer only; drain() sends
    // Cap latency: if capture ran ahead (bursty delivery), don't let the
    // backlog grow past ~0.5 s of audio — drop oldest whole packets.
    const int cap = kSampleRate * 2 / 2;         // 0.5 s of s16 mono
    if (acc_.size() > cap) acc_.remove(0, (acc_.size() - cap) & ~255);
}

// Emit packets on a real-time schedule: the number that SHOULD have gone out
// by now is elapsed * rate / samplesPerPkt. Send until caught up (or the
// buffer runs dry), so bursty capture becomes a steady ~55 pkt/s stream.
void TripAudio::drain() {
    if (!sock_) return;
    // Jitter buffer: build ~120 ms of cushion before the first packet (and
    // after any underrun), so brief pw-record delivery stalls don't starve
    // the radio's transmit buffer — the cause of the residual chops.
    constexpr int kPrebuffer = kSampleRate * 2 * 120 / 1000;   // ~120 ms s16
    if (!streaming_) {
        if (acc_.size() < kPrebuffer) return;
        streaming_ = true;
        clock_.restart();
        paced_ = 0;
    }
    const qint64 ns = clock_.nsecsElapsed();
    const quint64 due =
        quint64(ns * kSampleRate / (qint64(kSamplesPerPkt) * 1000000000LL));
    int guard = 0;                               // cap catch-up per tick
    while (paced_ < due && acc_.size() >= 256 && guard++ < 3) {
        const QByteArray pkt = packetize(counter_, acc_.constData());
        sock_->writeDatagram(pkt, QHostAddress(host_), audioPort_);
        acc_.remove(0, 256);
        ++paced_;
        ++pkts_;
    }
    // Underrun: behind schedule but the buffer's dry. Forfeit the missed
    // slots (accept a tiny gap) rather than bursting to catch up later.
    if (paced_ < due && acc_.size() < 256) paced_ = due;
}

} // namespace ttc
