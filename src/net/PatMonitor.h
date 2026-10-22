// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QObject>
#include <QString>
#include <QTimer>

class QWebSocket;

namespace ttc {

// Watches the Winlink client Pat (getpat.io, la5nta/pat) through its own
// web GUI's websocket (ws://localhost:8080/ws) — read-only: the console
// never drives Pat, it only needs to know when a Winlink session is
// dialing or connected, so it can hand the SignaLink over and say so.
// Pat pushes JSON objects (api/wshub.go):
//   {"Status":{"connected":bool,"dialing":bool,"remote_addr":"W8XX",...}}
//   {"Progress":{"bytes_transferred":n,"bytes_total":n,"sending":bool,
//                "receiving":bool,"done":bool,"subject":"..."}}
//   {"Ping":true}  {"LogLine":"..."}  {"Notification":...}
// Pat not running = no socket: retried every 10 s, quietly.
class PatMonitor : public QObject {
    Q_OBJECT
public:
    explicit PatMonitor(QObject* parent = nullptr);
    ~PatMonitor() override;

    void setEnabled(bool on, const QString& url = QStringLiteral("ws://127.0.0.1:8080/ws"));
    bool enabled() const { return enabled_; }
    bool active() const { return dialing_ || connected_; }   // a session on the air
    bool patUp() const { return patUp_; }
    QString remote() const { return remote_; }
    QString progressText() const { return progress_; }

    void handleMessage(const QString& json);       // public for wltest

signals:
    void sessionChanged(bool active, const QString& remote);
    void progressChanged(const QString& text);
    void patUpChanged(bool up);

private:
    void connectNow();
    void setPatUp(bool up);

    QWebSocket* ws_ = nullptr;
    QTimer retry_;
    QString url_;
    bool enabled_ = false, patUp_ = false;
    bool dialing_ = false, connected_ = false;
    QString remote_, progress_;
};

} // namespace ttc
