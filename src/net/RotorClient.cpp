// SPDX-License-Identifier: GPL-2.0-or-later
#include "net/RotorClient.h"

#include <QDateTime>
#include <QTimer>

namespace ttc {

// A slow rotor (DCU-3 at 4800 Bd) can leave replies from a previous
// backlog in flight; matched against new commands they read the
// elevation line as azimuth and the needle snaps to North. When the
// front command has waited this long, drop the connection — a
// reconnect is the only way to guarantee both sides start clean.
static constexpr qint64 kStaleMs = 4000;

RotorClient::RotorClient(QObject* parent) : QObject(parent) {
    timer_ = new QTimer(this);
    timer_->setInterval(1000);
    connect(timer_, &QTimer::timeout, this, &RotorClient::poll);
    connect(&sock_, &QTcpSocket::readyRead, this, &RotorClient::onReadyRead);
    connect(&sock_, &QTcpSocket::connected, this, [this] {
        emit connectedChanged(true);
        poll();
    });
    connect(&sock_, &QTcpSocket::disconnected, this, [this] {
        queue_.clear();
        gotLines_.clear();
        buf_.clear();
        az_ = -1.0;
        emit azimuthChanged(-1.0);
        emit connectedChanged(false);
    });
    connect(&sock_, &QTcpSocket::errorOccurred, this,
            [this](QAbstractSocket::SocketError) {
                queue_.clear();
                gotLines_.clear();
                buf_.clear();
            });
}

void RotorClient::setEndpoint(const QString& host, quint16 port) {
    host_ = host;
    port_ = port;
}

void RotorClient::setActive(bool on) {
    active_ = on;
    if (on) {
        timer_->start();
        if (sock_.state() == QAbstractSocket::UnconnectedState)
            sock_.connectToHost(host_, port_);
    } else {
        timer_->stop();
        sock_.disconnectFromHost();
        if (sock_.state() != QAbstractSocket::UnconnectedState)
            sock_.abort();
    }
}

void RotorClient::poll() {
    if (!active_) return;
    if (sock_.state() == QAbstractSocket::UnconnectedState) {
        sock_.connectToHost(host_, port_);        // quiet auto-reconnect
        return;
    }
    if (sock_.state() != QAbstractSocket::ConnectedState) return;
    if (!queue_.isEmpty()) {
        if (QDateTime::currentMSecsSinceEpoch() - frontSentMs_ > kStaleMs)
            sock_.abort();                        // resync via reconnect
        return;                                   // one poll in flight max
    }
    send(QStringLiteral("p"), 2);                 // az + el lines
}

void RotorClient::send(const QString& cmd, int expectLines) {
    if (sock_.state() != QAbstractSocket::ConnectedState) return;
    if (queue_.isEmpty())
        frontSentMs_ = QDateTime::currentMSecsSinceEpoch();
    queue_.push_back({cmd, expectLines});
    sock_.write((cmd + QLatin1Char('\n')).toLatin1());
}

void RotorClient::turnTo(double azDeg) {
    while (azDeg < 0.0) azDeg += 360.0;
    while (azDeg >= 360.0) azDeg -= 360.0;
    target_ = azDeg;
    send(QString("P %1 0").arg(azDeg, 0, 'f', 1), 1);   // RPRT n
}

void RotorClient::stop() {
    target_ = -1.0;
    send(QStringLiteral("S"), 1);
}

void RotorClient::onReadyRead() {
    buf_ += sock_.readAll();
    int nl;
    while ((nl = buf_.indexOf('\n')) >= 0) {
        const QString line = QString::fromLatin1(buf_.left(nl)).trimmed();
        buf_.remove(0, nl + 1);
        if (queue_.isEmpty()) continue;            // unsolicited: drop
        // An error report ends any command regardless of expected lines.
        const bool rprt = line.startsWith(QLatin1String("RPRT"));
        gotLines_ << line;
        if (!rprt && gotLines_.size() < queue_.first().lines) continue;
        const Pending done = queue_.takeFirst();
        if (!queue_.isEmpty())
            frontSentMs_ = QDateTime::currentMSecsSinceEpoch();
        const QStringList lines = gotLines_;
        gotLines_.clear();
        // A position reply must be exactly az + el, both numeric and in
        // range — a desynced stream (elevation read as azimuth) fails
        // this and is dropped instead of snapping the needle to North.
        if (done.cmd == QLatin1String("p") && !rprt && lines.size() == 2) {
            bool okAz = false, okEl = false;
            const double az = lines[0].toDouble(&okAz);
            const double el = lines[1].toDouble(&okEl);
            if (okAz && okEl && az >= 0.0 && az < 360.0
                && el >= -90.0 && el <= 90.0 && az != az_) {
                az_ = az;
                emit azimuthChanged(az_);
            }
        }
    }
}

} // namespace ttc
