// SPDX-License-Identifier: GPL-2.0-or-later
#include "net/SntpCheck.h"

#include <QDateTime>
#include <QHostInfo>
#include <QNetworkDatagram>
#include <QTimer>
#include <QUdpSocket>

namespace ttc {

namespace {
constexpr qint64 kNtpToUnixSecs = 2208988800LL;   // 1900 -> 1970

void putTs(QByteArray& b, int at, qint64 ms) {
    const quint64 secs = quint64(ms / 1000 + kNtpToUnixSecs);
    const quint64 frac = quint64((ms % 1000) * 4294967296.0 / 1000.0);
    for (int i = 0; i < 4; ++i) b[at + i] = char((secs >> (24 - 8 * i)) & 0xff);
    for (int i = 0; i < 4; ++i) b[at + 4 + i] = char((frac >> (24 - 8 * i)) & 0xff);
}

qint64 getTs(const QByteArray& b, int at) {
    quint64 secs = 0, frac = 0;
    for (int i = 0; i < 4; ++i) secs = (secs << 8) | quint8(b[at + i]);
    for (int i = 0; i < 4; ++i) frac = (frac << 8) | quint8(b[at + 4 + i]);
    return qint64(secs - kNtpToUnixSecs) * 1000 + qint64(frac * 1000.0 / 4294967296.0);
}
} // namespace

SntpCheck::SntpCheck(QObject* parent) : QObject(parent) {}

QByteArray SntpCheck::request(qint64 t1Ms) {
    QByteArray b(48, '\0');
    b[0] = char(0x23);                   // LI 0, version 4, mode 3 (client)
    putTs(b, 40, t1Ms);                  // transmit timestamp
    return b;
}

bool SntpCheck::offsetMs(const QByteArray& r, qint64 t1, qint64 t4, qint64* off) {
    if (r.size() < 48) return false;
    const int mode = quint8(r[0]) & 0x7;
    const int stratum = quint8(r[1]);
    if (mode != 4 || stratum == 0 || stratum > 15) return false;   // not a server answer
    const qint64 t2 = getTs(r, 32);      // server receive
    const qint64 t3 = getTs(r, 40);      // server transmit
    *off = ((t2 - t1) + (t3 - t4)) / 2;
    return true;
}

void SntpCheck::check(const QString& server) {
    server_ = server;
    if (!sock_) {
        sock_ = new QUdpSocket(this);
        timeout_ = new QTimer(this);
        timeout_->setSingleShot(true);
        connect(timeout_, &QTimer::timeout, this, [this] { emit result(false, 0, server_); });
        connect(sock_, &QUdpSocket::readyRead, this, [this] {
            while (sock_->hasPendingDatagrams()) {
                const QByteArray r = sock_->receiveDatagram().data();
                const qint64 t4 = QDateTime::currentMSecsSinceEpoch();
                qint64 off = 0;
                if (!timeout_->isActive() || !offsetMs(r, t1_, t4, &off)) continue;
                timeout_->stop();
                emit result(true, off, server_);
            }
        });
        sock_->bind();
    }
    QHostInfo::lookupHost(server, this, [this](const QHostInfo& hi) {
        for (const QHostAddress& a : hi.addresses()) {
            if (a.protocol() != QAbstractSocket::IPv4Protocol) continue;
            t1_ = QDateTime::currentMSecsSinceEpoch();
            sock_->writeDatagram(request(t1_), a, 123);
            timeout_->start(4000);
            return;
        }
        emit result(false, 0, server_);
    });
}

} // namespace ttc
