// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QByteArray>
#include <QObject>
#include <QString>

class QUdpSocket;
class QTimer;

namespace ttc {

// "Is the PC clock right?" — one SNTP query (RFC 4330) to a public NTP
// pool, nothing more. Contest log checkers and FT8 both punish a clock
// that is seconds off; the console only WARNS (it never sets the clock —
// that is chrony/systemd-timesyncd's job and needs root).
class SntpCheck : public QObject {
    Q_OBJECT
public:
    explicit SntpCheck(QObject* parent = nullptr);
    void check(const QString& server = QStringLiteral("pool.ntp.org"));

    // Pure: offset of the local clock in ms (positive = PC is BEHIND the
    // server) from a 48-byte server reply and the local send/receive
    // times in ms since the Unix epoch. False on a malformed reply.
    static bool offsetMs(const QByteArray& reply, qint64 t1Ms, qint64 t4Ms,
                         qint64* offset);
    // Pure: the 48-byte client request.
    static QByteArray request(qint64 t1Ms);

signals:
    void result(bool ok, qint64 offsetMs, const QString& server);

private:
    QUdpSocket* sock_ = nullptr;
    QTimer* timeout_ = nullptr;
    qint64 t1_ = 0;
    QString server_;
};

} // namespace ttc
