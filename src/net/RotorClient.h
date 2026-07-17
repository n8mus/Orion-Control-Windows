// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QObject>
#include <QString>
#include <QTcpSocket>
#include <QVector>

class QTimer;

namespace ttc {

// rotctld (hamlib NET rotctl, default :4533) client — same single-master
// interop pattern as rig control on :4532: anything that can drive a
// rotator daemon can share the antenna. Text protocol: "p\n" reads
// azimuth+elevation (two lines), "P az el\n" turns, "S\n" stops; errors
// come back as "RPRT -n". One command is in flight at a time; replies
// are matched to a queue of expected line counts.
class RotorClient : public QObject {
    Q_OBJECT
public:
    explicit RotorClient(QObject* parent = nullptr);

    void setEndpoint(const QString& host, quint16 port);
    void setActive(bool on);               // connect + 1 s position polling
    bool connected() const { return sock_.state() == QAbstractSocket::ConnectedState; }

    double azimuth() const { return az_; } // last reported, -1 unknown
    double target()  const { return target_; }

    void turnTo(double azDeg);             // P az 0
    void stop();                           // S

signals:
    void azimuthChanged(double azDeg);     // -1 = lost/unknown
    void connectedChanged(bool on);

private:
    void poll();
    void send(const QString& cmd, int expectLines);
    void onReadyRead();

    QTcpSocket sock_;
    QTimer* timer_ = nullptr;
    QString host_ = QStringLiteral("127.0.0.1");
    quint16 port_ = 4533;
    bool    active_ = false;
    double  az_ = -1.0, target_ = -1.0;

    struct Pending { QString cmd; int lines; };
    QVector<Pending> queue_;               // [0] is in flight
    QStringList gotLines_;
    QByteArray buf_;
    qint64 frontSentMs_ = 0;               // when queue_[0] hit the wire
};

} // namespace ttc
