// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QHash>
#include <QJsonValue>
#include <functional>
#include <QObject>
#include <QTimer>
#include <QVector>

#include "net/SpotClient.h"   // Spot

class QWebSocket;

namespace ttc {

// FreeDV Reporter (qso.freedv.org) as a spot source, view-only: who is on
// FreeDV/RADE right now and where. The protocol is Socket.IO v4 over a
// WebSocket, spoken by hand as AetherSDR's FreeDvClient (GPL-3, KK7GWY and
// contributors) and freedv-backend's SocketIoClient (BSD-2) do:
//   server "0{sid,pingInterval,...}"  -> we "40{role:view,protocol_version:2}"
//   server "2" (ping)                 -> we "3" (pong); EIO v4 pings are
//                                        the server's, the client only answers
//   server '42["event",{...}]'        -> station table / spots
// A station table keyed by sid follows new_connection / freq_change /
// tx_report / remove_connection (frequency in Hz, last_tx ISO time).
// Every online station with a callsign and an HF/6 m frequency is a spot
// (kind 'V') — the spot table's "where is FreeDV right now", every band:
//   ON AIR          tx_report transmitting=true      (band map too)
//   TX n min ago    last_tx within 20 min            (band map too)
//   listening       on a frequency, quiet longer     (spot table only —
//                   a dozen of them parked on 14.236 would bury the map)
// plus, AetherSDR's rule, rx_report with a decoded callsign -> the HEARD
// station at the receiver's frequency with the SNR (20 min). Unlike
// AetherSDR the snapshot on connect counts too: last_tx carries the real
// time, so the table is full the moment it connects (measured: 33
// stations with a frequency on a weekday morning, two on the air).
class FreeDvReporter : public QObject {
    Q_OBJECT
public:
    explicit FreeDvReporter(QObject* parent = nullptr);
    ~FreeDvReporter() override;

    void setEnabled(bool on);
    bool enabled() const { return enabled_; }

    // Reporting (freedv-gui's role "report"): this station appears on the
    // map with call/grid/frequency, its TX state, and what it decodes.
    // Off = view-only. A change reconnects (the role is set at connect).
    // txCapable false = rx_only. The software name shows in the list.
    void setReporting(bool on, const QString& call, const QString& grid,
                      const QString& software, bool txCapable);
    bool reporting() const { return reporting_; }
    // While reporting: dial (Hz), keyed or not, and a decode — callsign
    // from the end of over, or "" for "receiving RADE, no call yet".
    void reportFrequency(qint64 hz);
    void reportTransmit(bool tx);
    void reportHeard(const QString& call, int snrDb);
    QString authPayload() const;                   // the "40{...}" connect, for tests

    // The FreeDV window's station list (freedv-gui's Reporter columns).
    struct StationInfo {
        QString sid, call, grid, version, mode, message;
        qint64 hz = 0;
        qint64 lastTx = 0, lastUpdate = 0;         // epoch secs, 0 = never
        bool transmitting = false, rxOnly = false;
        QString lastRxCall;                        // what THIS station last decoded
        int lastRxSnr = 0;
        qint64 lastRxAt = 0;
    };
    QVector<StationInfo> stations() const;
    // While reporting: our one-line status message (freedv-gui's "Msg"),
    // and asking another station to come to our frequency.
    void sendStatusMessage(const QString& text);
    void requestQsy(const QString& sid, qint64 hz, const QString& message);
    QVector<Spot> spots() const;
    int stationCount() const { return int(stations_.size()); }

    // One Engine.IO text frame (public for freedvtest). Replies go out
    // through send(); events update the table and emit spotsChanged.
    void handleFrame(const QString& frame);
    std::function<void(const QString&)> send;      // default: the socket

    static constexpr qint64 kSpotLifeSecs = 1200;  // heard spots; "lately" on the map
    // Test clock (epoch secs); null = the real one.
    std::function<qint64()> now;

signals:
    void spotsChanged();
    // Someone asked US to QSY (only reaches reporting clients).
    void qsyRequested(const QString& fromCall, qint64 hz, const QString& message);
    void statusChanged(const QString& s);

private:
    using Station = StationInfo;
    void connectNow();
    void handleEvent(const QString& name, const QJsonObject& o, bool replay);
    void addHeard(const QString& call, qint64 hz, const QString& comment);
    static qint64 isoSecs(const QJsonValue& v);
    void expire();

    void sendEvent(const QString& name, const QJsonObject& o);
    void resendState();                            // after (re)connect
    bool reporting_ = false, txCapable_ = true, fullyConnected_ = false;
    QString call_, grid_, software_;
    qint64 repHz_ = 0;
    QString repMsg_;
    bool repTx_ = false;
    QWebSocket* ws_ = nullptr;
    QTimer retry_, alive_, expire_;
    int backoffS_ = 5;
    bool enabled_ = false;
    QHash<QString, Station> stations_;             // by sid
    QHash<QString, Spot> heard_;                   // rx_report finds, by call
};

} // namespace ttc
