// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QHash>
#include <QObject>
#include <QString>
#include <QTcpServer>
#include <QVector>

class QTcpSocket;

namespace ttc {

// Local RBN: serves the console's own CW-skimmer finds over the classic
// DX-cluster telnet protocol (default 127.0.0.1:7300), so cqrlog's
// cluster window — or anything else that can log into a cluster node —
// can consume them. Same single-master interop pattern as rigctld :4532
// and cwdaemon :6789.
//
// Protocol kept to what parsers actually read: a login prompt on
// connect, then one spot per line:
//   DX de N8EM-#:   14012.0  W1AW         CW 25 WPM   1234Z
// Spots are throttled per call (2 min) since the skimmer re-announces
// every time a station IDs again.
class SkimServer : public QObject {
    Q_OBJECT
public:
    explicit SkimServer(QObject* parent = nullptr);
    ~SkimServer() override;                // detach client handlers first

    void setSpotterCall(const QString& call) { spotter_ = call; }
    bool start(quint16 port);              // listen on 127.0.0.1
    void stop();
    bool active() const { return srv_.isListening(); }
    quint16 port() const { return srv_.serverPort(); }
    int clientCount() const { return clients_.size(); }

public slots:
    // Wire to SkimmerEngine::spotFound.
    void announce(const QString& call, qint64 hz, int wpm);
    // Relay one raw "DX de ..." line from the console's own cluster
    // connection, verbatim — original spotter and comment preserved. With
    // these wired, :7300 carries everything the console aggregates and a
    // single-node client (HRD, cqrlog) loses nothing by pointing here.
    // No throttle: the upstream node already paced it.
    void relayLine(const QString& line);
    // POTA activations, formatted cluster-style. The POTA poller re-lists
    // every active park each cycle, so these are throttled hard (10 min
    // per call) — about the cadence real cluster re-spots give POTA.
    void announcePota(const QString& call, qint64 hz, const QString& park);

private:
    void onNewConnection();
    void sendLine(QTcpSocket* c, const QString& line);

    QTcpServer srv_;
    QString spotter_ = QStringLiteral("N8EM");
    QVector<QTcpSocket*> clients_;         // logged-in or not, all get spots
    QHash<QString, qint64> lastSent_;      // call -> epoch (throttle)
};

} // namespace ttc
