// SPDX-License-Identifier: GPL-2.0-or-later
#include "radio/TripAudio.h"

#include <QHostAddress>
#include <QUdpSocket>
#include <cstdio>

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
    QStringList args{"--raw", "--format=s16", "--rate=7013", "--channels=1"};
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
    return true;
}

void TripAudio::stop() {
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
    if (!rec_ || !sock_) return;
    acc_ += rec_->readAllStandardOutput();
    // 128 samples * 2 bytes = 256 bytes per TRIP datagram.
    while (acc_.size() >= 256) {
        const QByteArray pkt = packetize(counter_, acc_.constData());
        sock_->writeDatagram(pkt, QHostAddress(host_), audioPort_);
        ++pkts_;
        acc_.remove(0, 256);
    }
}

} // namespace ttc
