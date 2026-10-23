// SPDX-License-Identifier: GPL-2.0-or-later
#include "net/SpotClient.h"

#include <QRegularExpression>
#include <QDateTime>
#include <QTimeZone>
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
// A SH/DX history line — the page the node prints when asked for the last
// N spots. It is NOT the live "DX de" form: the spotter moves to the end in
// angle brackets and the spot carries its own timestamp.
//   " 14340.0  W1AW/9      21-Sep-2026 2237Z  WAS IL TNX QSO      <IU6DVS>"
// Live-sampled 2026-09-21 from DXSpider (W3LPL), CC Cluster (K0XM) and
// AR-Cluster (K1TTT) — all three print this same shape, which is why one
// regex covers every node in the picker. K0XM clips the comment at ~25
// characters; nothing else differs.
const QRegularExpression kHistRe(
    QStringLiteral(R"(^\s*([0-9]+(?:\.[0-9]+)?)\s+([A-Z0-9/\-]{3,})\s+)"
                   R"((\d{1,2}-[A-Za-z]{3}-\d{4})\s+(\d{4})Z\s*)"
                   R"((.*?)\s*<([A-Z0-9/\-#]+)>\s*$)"),
    QRegularExpression::CaseInsensitiveOption);
constexpr int kBackfillSpots = 100;    // ~covers the 20-minute TTL on a busy node
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
        upSince_.start();
        loginSent_ = false;
        loginFallback_.start(3000);              // some nodes never say "login:"
        emit statusChanged(QString("spots: connected to %1").arg(host_));
    });
    connect(&sock_, &QTcpSocket::disconnected, this, [this] {
        emit statusChanged("spots: disconnected");
        scheduleReconnect();
    });
    connect(&sock_, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        emit statusChanged("spots: " + sock_.errorString());
        scheduleReconnect();
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
            afterLogin();
        }
    });
    pruneTimer_.setInterval(60000);
    connect(&pruneTimer_, &QTimer::timeout, this, &SpotClient::prune);
}

int SpotClient::retryDelayMs(int step) {
    static const int kDelays[] = {15000, 30000, 60000, 120000, 300000, 600000};
    constexpr int n = int(sizeof(kDelays) / sizeof(kDelays[0]));
    return kDelays[step < 0 ? 0 : step >= n ? n - 1 : step];
}

void SpotClient::scheduleReconnect() {
    if (!enabled_ || reconnect_.isActive()) return;   // error + disconnected = one retry
    if (upSince_.isValid() && upSince_.elapsed() >= 120000) retryStep_ = 0;   // it held: quick again
    upSince_.invalidate();
    const int ms = retryDelayMs(retryStep_++);
    reconnect_.start(ms);
    emit statusChanged(QString("spots: retry in %1").arg(ms >= 60000 ? QString("%1 min").arg(ms / 60000)
                                                                    : QString("%1 s").arg(ms / 1000)));
}

void SpotClient::configure(const QString& host, quint16 port, const QString& login) {
    if (host != host_ || port != port_) retryStep_ = 0;   // a new node starts fresh
    host_  = host;
    port_  = port;
    login_ = login;
}

void SpotClient::setEnabled(bool on) {
    if (on == enabled_) return;
    enabled_ = on;
    if (on) {
        retryStep_ = 0;                      // the operator's own switch-on retries quickly
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

// Both login paths (prompt seen, or the no-prompt fallback) end here.
// The node needs a moment to chew on the login before it will take
// commands, and the mode config goes first so SH/DX answers under the
// filters the operator actually wants.
void SpotClient::afterLogin() {
    QTimer::singleShot(1500, this, &SpotClient::sendModeConfig);
    QTimer::singleShot(2500, this, &SpotClient::requestBackfill);
}

// Ask for the node's recent-spot page. Without this a fresh connection
// shows nothing until live traffic happens to arrive, which on a quiet
// band reads as a broken feed (and left the window all POTA, since that
// feed arrives as one API snapshot). Spots older than the TTL are dropped
// at parse time, so this fills exactly the window the console already
// keeps — no stale page, no special case in prune().
void SpotClient::requestBackfill() {
    if (!loginSent_ || sock_.state() != QAbstractSocket::ConnectedState) return;
    sock_.write("SH/DX " + QByteArray::number(kBackfillSpots) + "\r\n");
}

void SpotClient::clear() {
    if (byCall_.isEmpty()) return;
    byCall_.clear();
    emit spotsChanged();
}

bool SpotClient::spotDx(const QString& call, qint64 hz,
                        const QString& comment) {
    if (!enabled_ || sock_.state() != QAbstractSocket::ConnectedState
        || !loginSent_)
        return false;
    const QString c = call.trimmed().toUpper();
    if (c.isEmpty() || hz <= 0) return false;
    QString line =
        QString("DX %1 %2").arg(hz / 1000.0, 0, 'f', 1).arg(c);
    const QString cm = comment.trimmed();
    if (!cm.isEmpty()) line += ' ' + cm;
    sock_.write(line.toLatin1() + "\r\n");
    emit statusChanged(QString("spotted %1 at %2 kHz")
                           .arg(c)
                           .arg(hz / 1000.0, 0, 'f', 1));
    return true;
}

// Both line shapes carry the same five facts in a different order; the
// kind/offset/park rules must not drift between them, so they live here.
// False = drop this spot (out of band, or the CW-RBN skimmer flood).
bool SpotClient::fill(Spot& s, const QString& kHz, const QString& call,
                      const QString& spotter, const QString& comment,
                      qint64 atSecs) {
    qint64 hz = static_cast<qint64>(std::llround(kHz.toDouble() * 1000.0));
    if (hz < 1800000 || hz > 54000000) return false;   // HF/6m sanity
    const QString up = comment.toUpper();
    s.call    = call.toUpper();
    s.atSecs  = atSecs;
    s.spotter = spotter.toUpper();
    s.comment = comment.trimmed();
    if (up.contains("FT8") || up.contains("FT4")) {
        s.kind = 'F';
        const auto o = kHzOffRe.match(up);             // dial + audio offset
        if (o.hasMatch()) hz += o.captured(1).toLongLong();
    } else if (s.spotter.endsWith("-#")) {
        return false;              // the CW-RBN flood riding in with SET/SKIMMER
    } else if (up.contains("POTA")) {
        s.kind = 'P';
        const auto r = kParkRe.match(up);
        if (r.hasMatch()) s.tag = r.captured(1);
    }
    s.hz = hz;
    return true;
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
    if (login_.trimmed().isEmpty()) {              // a node needs a call to log in
        emit statusChanged("spots: set your callsign (Setup → Station) to log in");
        return;
    }
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
            afterLogin();
        }
    }
    bool changed = false;
    int nl;
    while ((nl = lineBuf_.indexOf('\n')) >= 0) {
        const QString line = QString::fromLatin1(lineBuf_.left(nl)).trimmed();
        lineBuf_.remove(0, nl + 1);
        const auto m = kSpotRe.match(line);
        if (m.hasMatch()) {
            Spot s;
            if (!fill(s, m.captured(2), m.captured(3), m.captured(1),
                      m.captured(4), QDateTime::currentSecsSinceEpoch()))
                continue;
            byCall_[s.call] = s;
            changed = true;
            emit rawSpotLine(line);    // relay feed (:7300), original text
            continue;
        }
        // The SH/DX backfill page, one line per remembered spot. It is
        // deliberately NOT relayed to :7300: a reconnect re-asks for the
        // page, and the relay has no dedupe, so cqrlog's band map would
        // get the same hour of spots again on every cluster hiccup.
        const auto h = kHistRe.match(line);
        if (!h.hasMatch()) continue;
        const QDateTime when = QDateTime::fromString(
            h.captured(3) + ' ' + h.captured(4), QStringLiteral("d-MMM-yyyy hhmm"));
        if (!when.isValid()) continue;
        Spot s;
        if (!fill(s, h.captured(1), h.captured(2), h.captured(6), h.captured(5),
                  QDateTime(when.date(), when.time(), QTimeZone::utc())
                      .toSecsSinceEpoch()))
            continue;
        // Already too old to survive the next prune, or we already hold a
        // fresher sighting of this call (the live feed outranks the page).
        if (QDateTime::currentSecsSinceEpoch() - s.atSecs > ttlFor(s)) continue;
        const auto it = byCall_.constFind(s.call);
        if (it != byCall_.cend() && it->atSecs >= s.atSecs) continue;
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
