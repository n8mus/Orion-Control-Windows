// SPDX-License-Identifier: GPL-2.0-or-later
#include "log/WsjtxListener.h"

#include <QDataStream>
#include <QHostAddress>
#include <QNetworkDatagram>
#include <QSettings>
#include <QUdpSocket>

#include "log/LogDb.h"
#include "util/CtyLookup.h"

namespace ttc {

namespace {
constexpr quint32 kWsjtxMagic = 0xadbccbda;
constexpr quint32 kTypeLoggedAdif = 12;
} // namespace

WsjtxListener::WsjtxListener(LogDb* db, const CtyLookup* cty,
                             QObject* parent)
    : QObject(parent), db_(db), cty_(cty) {}

void WsjtxListener::start() {
    QSettings s;
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
        if (magic != kWsjtxMagic || type != kTypeLoggedAdif) continue;
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
