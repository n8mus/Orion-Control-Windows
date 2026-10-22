// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>

class QUdpSocket;

namespace ttc {

// PSK Reporter (pskreporter.info) for FreeDV: every callsign this station
// decodes from a RADE end of over is reported as "heard by N8EM", so the
// sender can see where he got out. The datagram is freedv-gui's exactly
// (freedv-backend src/reporting/pskreporter.cpp, BSD-2, Mooneer Salem):
// IPFIX v10 to report.pskreporter.info:4739/udp — a receiver template
// (callsign, locator, software) and a sender template (callsign, 5-byte
// frequency, SNR, mode "FREEDV", info source 1, flow start time), then
// the records; batched and sent every 5 minutes (or at 50), as there.
class PskReporter : public QObject {
    Q_OBJECT
public:
    explicit PskReporter(QObject* parent = nullptr);

    void setIdentity(const QString& call, const QString& grid, const QString& software);
    void setEnabled(bool on);
    bool enabled() const { return enabled_; }
    void addHeard(const QString& call, qint64 hz, int snrDb);
    void flush();                                  // send what is queued now
    int queued() const { return int(recs_.size()); }

    struct Rec { QString call; qint64 hz; int snr; qint64 atSecs; };
    // One datagram (public for psktest): header, templates, receiver and
    // sender records, each set padded to 4 bytes.
    static QByteArray encode(const QString& rxCall, const QString& rxGrid,
                             const QString& software, const QVector<Rec>& recs,
                             quint32 seq, quint32 randomId, qint64 nowSecs);

signals:
    void sent(int records);

private:
    QUdpSocket* sock_ = nullptr;
    QTimer timer_;
    bool enabled_ = false;
    QString call_, grid_, software_;
    QVector<Rec> recs_;
    quint32 seq_ = 0, id_ = 0;
};

} // namespace ttc
