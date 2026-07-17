// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include <QHash>
#include <QVector>

namespace ttc {

// One spot, newest sighting per callsign. kind: 'D' = classic DX-cluster,
// 'F' = FT8/FT4 (skimmer or human), 'P' = POTA activation. tag rides along
// for extra label text (the POTA park reference).
struct Spot {
    QString call;
    qint64  hz     = 0;
    qint64  atSecs = 0;      // epoch seconds of the last sighting
    char    kind   = 'D';
    QString tag;
    double  lat = 999.0;     // station location if known (999 = unknown);
    double  lon = 999.0;     // POTA park coords, or cty.dat country center
};

// Minimal DX-cluster telnet client (the same feed KE9NS PowerSDR's Spotter
// uses): connects to a cluster node, answers the login prompt with the
// operator's callsign, and parses "DX de SPOTTER: 14025.0 CALL ..." lines.
// Spots are deduped per call (latest freq wins) and expire after 20 minutes
// (10 for fast-churning FT8). Reconnects with backoff while enabled; fully
// quiet when disabled.
class SpotClient : public QObject {
    Q_OBJECT
public:
    explicit SpotClient(QObject* parent = nullptr);
    ~SpotClient() override;

    void configure(const QString& host, quint16 port, const QString& login);
    void setEnabled(bool on);
    bool enabled() const { return enabled_; }
    void clear();
    // Ask the node for FT8 skimmer spots (CC Cluster SET/SKIMMER + SET/FT8,
    // live-verified on VE7CC). The CW-RBN flood SET/SKIMMER also unleashes is
    // dropped at parse time, so this adds only FT8 to what the operator sees.
    // Takes effect immediately when connected; re-sent after every (re)login.
    void setFt8Wanted(bool on);
    // Server-side spotter-origin filter (CC Cluster SET/FILTER DOC/PASS,
    // live-verified): only spots MADE BY stations in these countries come
    // through — i.e. what's actually being heard in the operator's part of
    // the world. Comma-separated country prefixes ("K,VE,XE"); empty = all.
    void setSpotterFilter(const QString& ctyList);

    QVector<Spot> spots() const;                 // unexpired, unsorted

signals:
    void spotsChanged();
    void statusChanged(const QString& s);        // for the status bar

private:
    void openSocket();
    void onData();
    void prune();
    void sendModeConfig();

    QTcpSocket sock_;
    QTimer     reconnect_;                       // retry while enabled
    QTimer     pruneTimer_;
    QTimer     loginFallback_;                   // send login even w/o a prompt
    QString    host_  = "dxc.ve7cc.net";
    quint16    port_  = 23;
    QString    login_;
    bool       enabled_   = false;
    bool       loginSent_ = false;
    bool       ft8Wanted_ = false;
    QString    spotterCty_;                      // DOC/PASS list; empty = all
    QByteArray lineBuf_;
    QHash<QString, Spot> byCall_;
};

} // namespace ttc
