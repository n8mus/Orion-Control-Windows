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
constexpr quint32 kTypeStatus = 1;
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
        if (magic != kWsjtxMagic) continue;
        if (type == kTypeStatus) {
            // Status rides in constantly; the DX-call field changes the
            // moment a decode is clicked or a transmission starts.
            QByteArray id;
            in >> id;
            quint64 dial = 0;
            in >> dial;
            utf8Field(in);                          // mode
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
            if (dxCall.isEmpty() || dxCall == lastDx_) continue;
            lastDx_ = dxCall;
            emit dxChanged(dxCall, dxGrid);
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
