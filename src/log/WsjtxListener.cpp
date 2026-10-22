// SPDX-License-Identifier: GPL-2.0-or-later
#include "log/WsjtxListener.h"

#include <algorithm>

#include <QDataStream>
#include <QDateTime>
#include <QRegularExpression>
#include <QTimer>
#include <QHostAddress>
#include <QNetworkDatagram>
#include <QSettings>
#include <QUdpSocket>

#include "log/LogDb.h"
#include "util/CtyLookup.h"

namespace ttc {

namespace {
constexpr quint32 kWsjtxMagic = 0xadbccbda;
constexpr quint32 kTypeHeartbeat = 0;
constexpr quint32 kTypeStatus = 1;
constexpr quint32 kTypeDecode = 2;
constexpr quint32 kTypeClose = 6;
constexpr qint64  kHeardSecs = 180;       // a decode is drawn for 3 min
constexpr quint32 kTypeLoggedAdif = 12;

QString utf8Field(QDataStream& in) {
    QByteArray b;
    in >> b;
    return QString::fromUtf8(b);
}
} // namespace

WsjtxListener::WsjtxListener(LogDb* db, const CtyLookup* cty,
                             QObject* parent)
    : QObject(parent), db_(db), cty_(cty) {}

bool WsjtxListener::alive() const {
    return lastHeard_.isValid() && lastHeard_.elapsed() < 20000;
}

void WsjtxListener::noteState() {
    const bool a = alive(), t = transmitting();
    if (a == aliveSent_ && t == txSent_) return;
    aliveSent_ = a;
    txSent_ = t;
    emit stateChanged(a, t);
}

QVector<WsjtxListener::Heard> WsjtxListener::heard() const {
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QVector<Heard> out;
    for (const Heard& h : heard_)
        if (now - h.atSecs < kHeardSecs) out.push_back(h);
    return out;
}

bool WsjtxListener::classify(const QString& message, const QString& myCall,
                             Heard& out) {
    static const QRegularExpression grid4("^[A-R]{2}[0-9]{2}$");
    static const QRegularExpression callRe(
        "^(?=.*[0-9])(?=.*[A-Z])[A-Z0-9/]{3,}$");
    // "<K1ABC>" is a hashed call: still a call, minus the brackets.
    QStringList w = message.trimmed().toUpper().split(' ', Qt::SkipEmptyParts);
    for (QString& x : w)
        if (x.startsWith('<') && x.endsWith('>')) x = x.mid(1, x.size() - 2);
    if (w.size() < 2) return false;
    const auto isCall = [](const QString& x) {
        return callRe.match(x).hasMatch() && !grid4.match(x).hasMatch();
    };
    int at = -1;
    if (w[0] == "CQ") {
        // "CQ K1ABC FN42", "CQ DX K1ABC FN42", "CQ POTA K1ABC": the call
        // is the first word after CQ that looks like one.
        for (int i = 1; i < w.size() && i <= 2; ++i)
            if (isCall(w[i])) { at = i; break; }
        out.toMe = false;
    } else if (!myCall.isEmpty() && w[0] == myCall.toUpper() && isCall(w[1])) {
        at = 1;
        out.toMe = true;
    }
    if (at < 0) return false;
    out.call = w[at];
    out.grid = at + 1 < w.size() && grid4.match(w[at + 1]).hasMatch()
                   && w[at + 1] != "RR73" ? w[at + 1] : QString();
    out.message = message.trimmed();
    return true;
}

bool WsjtxListener::requestClose() {
    if (!sock_ || peerPort_ == 0 || clientId_.isEmpty()) return false;
    QByteArray out;
    QDataStream o(&out, QIODevice::WriteOnly);
    o.setByteOrder(QDataStream::BigEndian);
    o << kWsjtxMagic << schema_ << kTypeClose << clientId_;
    return sock_->writeDatagram(out, peer_, peerPort_) == out.size();
}

void WsjtxListener::start() {
    QSettings s;
    myCall_ = s.value("station/callsign", "").toString().trimmed().toUpper();
    // alive() runs off a clock: re-check it even when WSJT-X goes silent.
    auto* tick = new QTimer(this);
    tick->setInterval(2000);
    connect(tick, &QTimer::timeout, this, [this] { noteState(); });
    tick->start();
    if (!s.value("log/wsjtx", true).toBool()) return;
    const QString addr =
        s.value("log/wsjtxAddr", "224.0.0.1").toString().trimmed();
    const quint16 port = quint16(s.value("log/wsjtxPort", 2237).toUInt());
    sock_ = new QUdpSocket(this);
    // Share the port: WSJT-X's own clients (GridTracker) listen too.
    if (!sock_->bind(QHostAddress::AnyIPv4, port,
                     QUdpSocket::ShareAddress
                         | QUdpSocket::ReuseAddressHint))
        return;
    const QHostAddress group(addr);
    if (group.isMulticast()) sock_->joinMulticastGroup(group);
    connect(sock_, &QUdpSocket::readyRead, this,
            [this] { onDatagram(); });
}

void WsjtxListener::onDatagram() {
    while (sock_->hasPendingDatagrams()) {
        const QNetworkDatagram dg = sock_->receiveDatagram();
        QDataStream in(dg.data());
        in.setByteOrder(QDataStream::BigEndian);
        quint32 magic = 0, schema = 0, type = 0;
        in >> magic >> schema >> type;
        if (magic != kWsjtxMagic) continue;
        lastHeard_.start();
        peer_ = dg.senderAddress();
        peerPort_ = quint16(dg.senderPort());
        schema_ = std::min<quint32>(schema, 3);
        {
            QDataStream idIn(dg.data());
            idIn.setByteOrder(QDataStream::BigEndian);
            quint32 skip = 0;
            idIn >> skip >> skip >> skip >> clientId_;
        }
        if (type == kTypeClose) {
            lastHeard_.invalidate();
            transmitting_ = false;
            noteState();
            continue;
        }
        if (type == kTypeHeartbeat) {
            noteState();
            continue;
        }
        if (type == kTypeDecode) {
            QByteArray id;
            bool isNew = false;
            quint32 ms = 0, df = 0;
            qint32 snr = 0;
            double dt = 0;
            in >> id >> isNew >> ms >> snr >> dt >> df;
            utf8Field(in);                          // mode
            const QString msg = utf8Field(in);
            if (in.status() != QDataStream::Ok || !isNew || dialHz_ == 0)
                continue;                           // replays, or no dial yet
            Heard h;
            if (!classify(msg, myCall_, h)) continue;
            h.hz = qint64(dialHz_) + df;
            h.snr = snr;
            h.atSecs = QDateTime::currentSecsSinceEpoch();
            const qint64 now = h.atSecs;
            heard_.erase(std::remove_if(heard_.begin(), heard_.end(),
                             [&](const Heard& o) {
                                 return o.call == h.call
                                     || now - o.atSecs >= kHeardSecs;
                             }),
                         heard_.end());
            heard_.push_back(h);
            emit heardChanged();
            continue;
        }
        if (type == kTypeStatus) {
            // Status rides in constantly; the DX-call field changes the
            // moment a decode is clicked or a transmission starts.
            QByteArray id;
            in >> id;
            quint64 dial = 0;
            in >> dial;
            const QString wsMode = utf8Field(in).trimmed().toUpper();
            const QString dxCall = utf8Field(in).trimmed().toUpper();
            utf8Field(in);                          // report
            utf8Field(in);                          // tx mode
            bool txEnabled = false, transmitting = false, decoding = false;
            in >> txEnabled >> transmitting >> decoding;
            quint32 rxDf = 0, txDf = 0;
            in >> rxDf >> txDf;
            utf8Field(in);                          // DE call
            utf8Field(in);                          // DE grid
            const QString dxGrid = utf8Field(in).trimmed().toUpper();
            if (in.status() != QDataStream::Ok) continue;
            if (dial != dialHz_) {
                // A band change strands the old decodes off-screen at
                // stale frequencies: drop them.
                if (dialHz_ != 0 && !heard_.isEmpty()) {
                    heard_.clear();
                    emit heardChanged();
                }
                dialHz_ = dial;
            }
            transmitting_ = transmitting;
            noteState();
            (void)txEnabled; (void)decoding;
            if (dxCall.isEmpty() || dxCall == lastDx_) continue;
            lastDx_ = dxCall;
            emit dxChanged(dxCall, dxGrid, wsMode);
            continue;
        }
        if (type != kTypeLoggedAdif) continue;
        QByteArray id, adif;
        in >> id >> adif;                    // QByteArray = length-prefixed
        if (in.status() != QDataStream::Ok || adif.isEmpty()) continue;
        const auto recs = Adif::parseBytes(adif);
        if (recs.isEmpty()) continue;
        Qso q = LogDb::fromAdif(recs.first());
        if (q.call.isEmpty() || !q.tsUtc.isValid()) continue;
        if (q.country.isEmpty() && cty_) {
            CtyInfo ci;
            if (cty_->info(q.call, ci)) {
                q.country = ci.country;
                q.cqz = ci.cq;
                q.ituz = ci.itu;
            }
        }
        if (db_->hasNearDuplicate(q)) continue;   // WSJT-X resend, or both
                                                  // 2237 and a relay heard
        // The console pushes WSJT-X QSOs to the online logs itself
        // (operator's call 2026-09-01: GridTracker stays off). Set
        // log/wsjtxPush=false to hand that job back to GridTracker —
        // the QSO is then stored stamped already-uploaded.
        const bool push =
            QSettings().value("log/wsjtxPush", true).toBool();
        const qint64 rowId = db_->addQso(q);
        if (rowId < 0) continue;
        if (!push)
            for (const char* svc : {"lotw", "eqsl", "qrz", "club",
                                    "hrdlog"})
                db_->setUploadState(rowId, QLatin1String(svc), 'Y');
        emit qsoLogged(rowId, q.call, push);
    }
}

} // namespace ttc
