// SPDX-License-Identifier: GPL-2.0-or-later
#include "net/SpotClient.h"

#include <QRegularExpression>
#include <QDateTime>
#include <cmath>

namespace ttc {

namespace {
constexpr qint64 kSpotTtlSecs = 20 * 60;         // spots fade after 20 minutes
constexpr qint64 kFt8TtlSecs  = 10 * 60;         // FT8 churns much faster
constexpr int    kMaxSpots    = 1000;            // sized for the FT8 firehose

// "DX de W1AW-#:   14025.0  DL1XYZ   CW 25 dB   0123Z"  (freq is kHz)
const QRegularExpression kSpotRe(
    QStringLiteral(R"(^DX de\s+(\S+?):?\s+([0-9]+(?:\.[0-9]+)?)\s+([A-Z0-9/\-]{3,})\s*(.*)$)"),
    QRegularExpression::CaseInsensitiveOption);
// FT8 skimmer comments carry the audio offset ("FT8 -18 dB 1026 Hz"): add it
// to the dial frequency so the label lands on the station's actual RF instead
// of every FT8 spot piling up at the watering-hole dial.
const QRegularExpression kHzOffRe(QStringLiteral(R"((\d{2,4})\s*HZ\b)"));
// POTA park reference in a human spot comment ("POTA US-2654 ...").
const QRegularExpression kParkRe(QStringLiteral(R"(\b([A-Z0-9]{1,3}-\d{3,5})\b)"));
} // namespace

// Members destruct in REVERSE declaration order: the three QTimers die
// BEFORE sock_ (declared first). A connected socket's ~QTcpSocket then
// emits disconnected()/errorOccurred() mid-teardown, and the reconnect
// lambda called start() on an already-destroyed QTimer — a use-after-free
// planted at every exit with the cluster feed up, detected moments later
// as "corrupted double-linked list" inside the same destructor (the three
// 2026-07-16 cores; run to ground by ASan on the first pass). Sever our
// connections before any member dies; abort() closes without ceremony.
SpotClient::~SpotClient() {
    sock_.disconnect(this);
    sock_.abort();
}

SpotClient::SpotClient(QObject* parent) : QObject(parent) {
    connect(&sock_, &QTcpSocket::readyRead, this, &SpotClient::onData);
    connect(&sock_, &QTcpSocket::connected, this, [this] {
        loginSent_ = false;
        loginFallback_.start(3000);              // some nodes never say "login:"
        emit statusChanged(QString("spots: connected to %1").arg(host_));
    });
    connect(&sock_, &QTcpSocket::disconnected, this, [this] {
        if (enabled_) reconnect_.start(15000);
        emit statusChanged("spots: disconnected");
    });
    connect(&sock_, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        if (enabled_) reconnect_.start(15000);
        emit statusChanged("spots: " + sock_.errorString());
    });
    reconnect_.setSingleShot(true);
    connect(&reconnect_, &QTimer::timeout, this, [this] {
        if (enabled_) openSocket();
    });
    loginFallback_.setSingleShot(true);
    connect(&loginFallback_, &QTimer::timeout, this, [this] {
        if (!loginSent_ && sock_.state() == QAbstractSocket::ConnectedState) {
            sock_.write(login_.toLatin1() + "\r\n");
            loginSent_ = true;
            QTimer::singleShot(1500, this, &SpotClient::sendModeConfig);
        }
    });
    pruneTimer_.setInterval(60000);
    connect(&pruneTimer_, &QTimer::timeout, this, &SpotClient::prune);
}

void SpotClient::configure(const QString& host, quint16 port, const QString& login) {
    host_  = host;
    port_  = port;
    login_ = login;
}

void SpotClient::setEnabled(bool on) {
    if (on == enabled_) return;
    enabled_ = on;
    if (on) {
        openSocket();
        pruneTimer_.start();
    } else {
        reconnect_.stop();
        pruneTimer_.stop();
        sock_.abort();
        emit statusChanged("spots: off");
    }
}

void SpotClient::setFt8Wanted(bool on) {
    if (on == ft8Wanted_) return;
    ft8Wanted_ = on;
    sendModeConfig();                            // no-op unless logged in
}

void SpotClient::setSpotterFilter(const QString& ctyList) {
    if (ctyList == spotterCty_) return;
    spotterCty_ = ctyList;
    sendModeConfig();
}

// CC Cluster per-login config: FT8 spots are skimmer spots, so both switches
// are needed (SET/FT8 alone only passes the rare human-typed FT8 spot —
// live-verified on VE7CC), and the spotter-origin filter is enforced every
// login so the node's stored profile can't drift from the console setting.
void SpotClient::sendModeConfig() {
    if (!loginSent_ || sock_.state() != QAbstractSocket::ConnectedState) return;
    sock_.write(ft8Wanted_ ? "SET/SKIMMER\r\nSET/FT8\r\n"
                           : "SET/NOSKIMMER\r\nSET/NOFT8\r\n");
    sock_.write(spotterCty_.isEmpty()
        ? QByteArray("SET/FILTER DOC/OFF\r\n")
        : QByteArray("SET/FILTER DOC/PASS ") + spotterCty_.toLatin1() + "\r\n");
}

void SpotClient::clear() {
    if (byCall_.isEmpty()) return;
    byCall_.clear();
    emit spotsChanged();
}

static qint64 ttlFor(const Spot& s) {
    return s.kind == 'F' ? kFt8TtlSecs : kSpotTtlSecs;
}

QVector<Spot> SpotClient::spots() const {
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QVector<Spot> out;
    out.reserve(byCall_.size());
    for (const Spot& s : byCall_)
        if (now - s.atSecs <= ttlFor(s)) out.push_back(s);
    return out;
}

void SpotClient::openSocket() {
    sock_.abort();
    lineBuf_.clear();
    emit statusChanged(QString("spots: connecting to %1:%2").arg(host_).arg(port_));
    sock_.connectToHost(host_, port_);
}

void SpotClient::onData() {
    lineBuf_ += sock_.readAll();
    // Answer the login prompt (VE7CC: "Please enter your call:").
    if (!loginSent_) {
        const QString sofar = QString::fromLatin1(lineBuf_).toLower();
        if (sofar.contains("login") || sofar.contains("call")) {
            sock_.write(login_.toLatin1() + "\r\n");
            loginSent_ = true;
            QTimer::singleShot(1500, this, &SpotClient::sendModeConfig);
        }
    }
    bool changed = false;
    int nl;
    while ((nl = lineBuf_.indexOf('\n')) >= 0) {
        const QString line = QString::fromLatin1(lineBuf_.left(nl)).trimmed();
        lineBuf_.remove(0, nl + 1);
        const auto m = kSpotRe.match(line);
        if (!m.hasMatch()) continue;
        qint64 hz = static_cast<qint64>(std::llround(m.captured(2).toDouble() * 1000.0));
        if (hz < 1800000 || hz > 54000000) continue;   // HF/6m sanity
        const bool skimmer = m.captured(1).endsWith("-#");
        const QString comment = m.captured(4).toUpper();
        Spot s;
        s.call   = m.captured(3).toUpper();
        s.atSecs = QDateTime::currentSecsSinceEpoch();
        if (comment.contains("FT8") || comment.contains("FT4")) {
            s.kind = 'F';
            const auto o = kHzOffRe.match(comment);    // dial + audio offset
            if (o.hasMatch()) hz += o.captured(1).toLongLong();
        } else if (skimmer) {
            continue;              // the CW-RBN flood riding in with SET/SKIMMER
        } else if (comment.contains("POTA")) {
            s.kind = 'P';
            const auto r = kParkRe.match(comment);
            if (r.hasMatch()) s.tag = r.captured(1);
        }
        s.hz = hz;
        byCall_[s.call] = s;
        changed = true;
    }
    if (byCall_.size() > kMaxSpots) prune();
    if (changed) emit spotsChanged();
}

void SpotClient::prune() {
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    bool changed = false;
    for (auto it = byCall_.begin(); it != byCall_.end();) {
        if (now - it->atSecs > ttlFor(*it)) {
            it = byCall_.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    // Still over the cap (a very busy contest weekend): drop the oldest.
    while (byCall_.size() > kMaxSpots) {
        auto oldest = byCall_.begin();
        for (auto it = byCall_.begin(); it != byCall_.end(); ++it)
            if (it->atSecs < oldest->atSecs) oldest = it;
        byCall_.erase(oldest);
        changed = true;
    }
    if (changed) emit spotsChanged();
}

} // namespace ttc
