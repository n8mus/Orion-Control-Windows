// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/PatMonitor.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QWebSocket>

namespace ttc {

PatMonitor::PatMonitor(QObject* parent) : QObject(parent) {
    retry_.setSingleShot(true);
    retry_.setInterval(10000);
    connect(&retry_, &QTimer::timeout, this, &PatMonitor::connectNow);
}

PatMonitor::~PatMonitor() { delete ws_; }

void PatMonitor::setEnabled(bool on, const QString& url) {
    url_ = url;
    if (on == enabled_) return;
    enabled_ = on;
    if (on) { connectNow(); return; }
    retry_.stop();
    if (ws_) {
        ws_->disconnect(this);
        ws_->abort();
        ws_->deleteLater();
        ws_ = nullptr;
    }
    setPatUp(false);
}

void PatMonitor::setPatUp(bool up) {
    if (!up && active()) {                         // Pat gone mid-session
        dialing_ = connected_ = false;
        emit sessionChanged(false, remote_);
    }
    if (up == patUp_) return;
    patUp_ = up;
    emit patUpChanged(up);
}

void PatMonitor::connectNow() {
    if (!enabled_) return;
    if (!ws_) {
        ws_ = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
        connect(ws_, &QWebSocket::textMessageReceived, this, &PatMonitor::handleMessage);
        connect(ws_, &QWebSocket::connected, this, [this] { setPatUp(true); });
        connect(ws_, &QWebSocket::disconnected, this, [this] {
            setPatUp(false);
            if (enabled_) retry_.start();
        });
    }
    ws_->open(QUrl(url_));
}

void PatMonitor::handleMessage(const QString& json) {
    const QJsonObject o = QJsonDocument::fromJson(json.toUtf8()).object();
    if (o.contains("Status")) {
        const QJsonObject s = o.value("Status").toObject();
        const bool wasActive = active();
        dialing_ = s.value("dialing").toBool();
        connected_ = s.value("connected").toBool();
        const QString r = s.value("remote_addr").toString();
        if (!wasActive && active()) remote_ = r;   // a new session: whoever it is
        else if (!r.isEmpty()) remote_ = r;        // ending: keep who it was
        if (!active()) progress_.clear();
        if (active() != wasActive) emit sessionChanged(active(), remote_);
    } else if (o.contains("Progress")) {
        const QJsonObject p = o.value("Progress").toObject();
        const int done = p.value("bytes_transferred").toInt();
        const int total = p.value("bytes_total").toInt();
        const QString dir = p.value("sending").toBool() ? QStringLiteral("sending")
                          : p.value("receiving").toBool() ? QStringLiteral("receiving")
                                                          : QString();
        progress_ = p.value("done").toBool() || dir.isEmpty()
            ? QString()
            : QString("%1 %2/%3 kB").arg(dir).arg(done / 1024.0, 0, 'f', 1)
                  .arg(total / 1024.0, 0, 'f', 1);
        emit progressChanged(progress_);
    }
}

} // namespace ttc
