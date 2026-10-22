// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/FreeDvReporter.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QWebSocket>
#include <algorithm>
#include <cmath>

namespace ttc {

namespace {
const char* kUrl = "wss://qso.freedv.org/socket.io/?EIO=4&transport=websocket";
qint64 nowSecs() { return QDateTime::currentSecsSinceEpoch(); }
}  // namespace

FreeDvReporter::FreeDvReporter(QObject* parent) : QObject(parent) {
    send = [this](const QString& t) { if (ws_) ws_->sendTextMessage(t); };
    retry_.setSingleShot(true);
    connect(&retry_, &QTimer::timeout, this, &FreeDvReporter::connectNow);
    // No server ping within pingInterval + pingTimeout = a dead link
    // (freedv-backend's rule); re-armed on every frame.
    alive_.setSingleShot(true);
    alive_.setInterval(60000);
    connect(&alive_, &QTimer::timeout, this, [this] {
        if (ws_) ws_->abort();                     // -> disconnected -> retry
    });
    expire_.setInterval(30000);
    connect(&expire_, &QTimer::timeout, this, &FreeDvReporter::expire);
}

FreeDvReporter::~FreeDvReporter() { delete ws_; }

void FreeDvReporter::setEnabled(bool on) {
    if (on == enabled_) return;
    enabled_ = on;
    if (on) {
        backoffS_ = 5;
        expire_.start();
        connectNow();
        return;
    }
    retry_.stop();
    alive_.stop();
    expire_.stop();
    if (ws_) {
        ws_->disconnect(this);
        ws_->close();
        ws_->deleteLater();
        ws_ = nullptr;
    }
    stations_.clear();
    heard_.clear();
    emit spotsChanged();
    emit statusChanged("FreeDV Reporter: off");
}

void FreeDvReporter::connectNow() {
    if (!enabled_) return;
    if (!ws_) {
        ws_ = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
        connect(ws_, &QWebSocket::textMessageReceived, this, &FreeDvReporter::handleFrame);
        connect(ws_, &QWebSocket::connected, this, [this] {
            backoffS_ = 5;
            alive_.start();
        });
        connect(ws_, &QWebSocket::disconnected, this, [this] {
            alive_.stop();
            fullyConnected_ = false;
            stations_.clear();                     // a reconnect re-sends it all
            emit spotsChanged();
            if (!enabled_) return;
            emit statusChanged(QString("FreeDV Reporter: disconnected, retry in %1 s")
                                   .arg(backoffS_));
            retry_.start(backoffS_ * 1000);
            backoffS_ = std::min(backoffS_ * 2, 60);
        });
    }
    ws_->open(QUrl(QString::fromLatin1(kUrl)));
}

void FreeDvReporter::handleFrame(const QString& f) {
    alive_.start();
    if (f.isEmpty()) return;
    switch (f[0].toLatin1()) {
    case '0':                                      // EIO open -> SIO connect
        fullyConnected_ = false;
        send(authPayload());
        return;
    case '2':                                      // ping
        send(QStringLiteral("3"));
        return;
    case '4':
        break;
    default:                                       // 1 close, 3 pong, 6 noop
        return;
    }
    if (f.startsWith(QLatin1String("40"))) {
        emit statusChanged("FreeDV Reporter: connected");
        return;
    }
    if (f.startsWith(QLatin1String("41"))) return; // server-side disconnect
    if (!f.startsWith(QLatin1String("42"))) return;
    const QJsonDocument d = QJsonDocument::fromJson(f.mid(2).toUtf8());
    const QJsonArray a = d.array();
    if (a.size() < 1) return;
    const QString name = a.at(0).toString();
    if (name == QLatin1String("connection_successful")) {
        // freedv-backend sends nothing before this: the server is ready.
        fullyConnected_ = true;
        resendState();
        return;
    }
    static const QSet<QString> kStationEvents = {
        "bulk_update", "new_connection", "remove_connection", "freq_change",
        "tx_report", "rx_report", "message_update"};
    if (name == QLatin1String("bulk_update")) {
        // [[name,{...}], ...] — the snapshot on connect, replayed through
        // the same handlers.
        for (const QJsonValue& v : a.at(1).toArray()) {
            const QJsonArray p = v.toArray();
            handleEvent(p.at(0).toString(), p.at(1).toObject(), true);
        }
    } else {
        handleEvent(name, a.at(1).toObject(), false);
    }
    if (kStationEvents.contains(name)) emit spotsChanged();
}

QString FreeDvReporter::authPayload() const {
    QJsonObject o;
    if (reporting_ && !call_.isEmpty() && !grid_.isEmpty()) {
        o["role"] = "report";
        o["callsign"] = call_;
        o["grid_square"] = grid_;
        o["version"] = software_;
        o["rx_only"] = !txCapable_;
        o["os"] = "linux";
    } else {
        o["role"] = "view";
    }
    o["protocol_version"] = 2;
    return "40" + QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

void FreeDvReporter::setReporting(bool on, const QString& call, const QString& grid,
                                  const QString& software, bool txCapable) {
    const bool changed = on != reporting_ || call != call_ || grid != grid_
                      || txCapable != txCapable_ || software != software_;
    reporting_ = on;
    call_ = call.toUpper();
    grid_ = grid;
    software_ = software;
    txCapable_ = txCapable;
    if (changed && ws_ && enabled_) {              // the role is fixed at connect
        fullyConnected_ = false;
        ws_->abort();                              // -> disconnected -> retry
    }
}

void FreeDvReporter::sendEvent(const QString& name, const QJsonObject& o) {
    if (!reporting_ || !fullyConnected_) return;
    QJsonArray a{name, o};
    send("42" + QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact)));
}

void FreeDvReporter::resendState() {
    if (repHz_ > 0) sendEvent("freq_change", QJsonObject{{"freq", double(repHz_)}});
    sendEvent("tx_report", QJsonObject{{"mode", "RADEV1"}, {"transmitting", repTx_}});
    if (!repMsg_.isEmpty()) sendEvent("message_update", QJsonObject{{"message", repMsg_}});
}

void FreeDvReporter::sendStatusMessage(const QString& text) {
    repMsg_ = text;
    sendEvent("message_update", QJsonObject{{"message", text}});
}

void FreeDvReporter::requestQsy(const QString& sid, qint64 hz, const QString& message) {
    sendEvent("qsy_request", QJsonObject{{"dest_sid", sid}, {"message", message},
                                         {"frequency", double(hz)}});
}

QVector<FreeDvReporter::StationInfo> FreeDvReporter::stations() const {
    QVector<StationInfo> v;
    for (const Station& st : stations_)
        if (!st.call.isEmpty()) v.push_back(st);
    return v;
}

void FreeDvReporter::reportFrequency(qint64 hz) {
    if (hz == repHz_) return;
    repHz_ = hz;
    sendEvent("freq_change", QJsonObject{{"freq", double(hz)}});
}

void FreeDvReporter::reportTransmit(bool tx) {
    if (tx == repTx_) return;
    repTx_ = tx;
    sendEvent("tx_report", QJsonObject{{"mode", "RADEV1"}, {"transmitting", tx}});
}

void FreeDvReporter::reportHeard(const QString& call, int snrDb) {
    sendEvent("rx_report", QJsonObject{{"callsign", call.toUpper()}, {"mode", "RADEV1"},
                                       {"snr", snrDb}});
}

qint64 FreeDvReporter::isoSecs(const QJsonValue& v) {
    // "2026-10-15T15:17:43.697229+00:00" (microseconds; null = never).
    if (!v.isString()) return 0;
    QString t = v.toString();
    const int dot = t.indexOf('.');
    if (dot > 0) {                                 // trim to ms for Qt
        int end = dot + 1;
        while (end < t.size() && t[end].isDigit()) ++end;
        t = t.left(std::min(end, dot + 4)) + t.mid(end);
    }
    const QDateTime d = QDateTime::fromString(t, Qt::ISODateWithMs);
    return d.isValid() ? d.toSecsSinceEpoch() : 0;
}

void FreeDvReporter::handleEvent(const QString& name, const QJsonObject& o, bool) {
    const QString sid = o.value("sid").toString();
    const auto fill = [&](Station& st) {
        const QString c = o.value("callsign").toString().toUpper();
        if (!c.isEmpty()) st.call = c;
        const QString g = o.value("grid_square").toString();
        if (!g.isEmpty()) st.grid = g;
        if (o.contains("rx_only")) st.rxOnly = o.value("rx_only").toBool();
        const QString v = o.value("version").toString();
        if (!v.isEmpty()) st.version = v;
        st.sid = sid;
        if (const qint64 u = isoSecs(o.value("last_update"))) st.lastUpdate = u;
    };
    if (name == QLatin1String("new_connection")) {
        fill(stations_[sid]);
    } else if (name == QLatin1String("remove_connection")) {
        stations_.remove(sid);
    } else if (name == QLatin1String("freq_change")) {
        Station& st = stations_[sid];
        fill(st);
        st.hz = qint64(o.value("freq").toDouble());
    } else if (name == QLatin1String("tx_report")) {
        Station& st = stations_[sid];
        fill(st);
        const QString m = o.value("mode").toString();
        if (!m.isEmpty()) st.mode = m;
        st.transmitting = o.value("transmitting").toBool();
        if (const qint64 t = isoSecs(o.value("last_tx"))) st.lastTx = t;
        if (st.transmitting) st.lastTx = std::max(st.lastTx, now ? now() : QDateTime::currentSecsSinceEpoch());
    } else if (name == QLatin1String("message_update")) {
        const auto it = stations_.find(sid);
        if (it != stations_.end()) it->message = o.value("message").toString();
    } else if (name == QLatin1String("qsy_request")) {
        emit qsyRequested(o.value("callsign").toString().toUpper(),
                          qint64(o.value("frequency").toDouble()), o.value("message").toString());
    } else if (name == QLatin1String("rx_report")) {
        const QString heard = o.value("callsign").toString().toUpper();
        const auto rit = stations_.find(sid);      // the RECEIVER's sid
        if (rit != stations_.end() && !heard.isEmpty()) {
            rit->lastRxCall = heard;
            rit->lastRxSnr = int(std::lround(o.value("snr").toDouble()));
            rit->lastRxAt = now ? now() : QDateTime::currentSecsSinceEpoch();
        }
        const auto it = stations_.constFind(sid);
        if (heard.isEmpty() || it == stations_.constEnd()) return;
        const QString rxCall = o.value("receiver_callsign").toString().toUpper();
        addHeard(heard, it->hz,
                 QString("%1 heard by %2 %3 dB").arg(o.value("mode").toString(), rxCall)
                     .arg(o.value("snr").toDouble(), 0, 'f', 0));
    }
}

void FreeDvReporter::addHeard(const QString& call, qint64 hz, const QString& comment) {
    // HF/6 m sanity, as the cluster parser and POTA clamp.
    if (call.isEmpty() || hz < 1800000 || hz > 54000000) return;
    Spot s;
    s.call = call;
    s.hz = hz;
    s.atSecs = now ? now() : QDateTime::currentSecsSinceEpoch();
    s.kind = 'V';
    s.tag = QStringLiteral("FreeDV");
    s.spotter = QStringLiteral("FreeDV Reporter");
    s.comment = comment;
    heard_[call] = s;
}

void FreeDvReporter::expire() {
    const qint64 t = now ? now() : QDateTime::currentSecsSinceEpoch();
    for (auto it = heard_.begin(); it != heard_.end();) {
        if (t - it->atSecs > kSpotLifeSecs) it = heard_.erase(it);
        else ++it;
    }
    emit spotsChanged();                           // "TX n min ago" moves on
}

QVector<Spot> FreeDvReporter::spots() const {
    const qint64 t = now ? now() : QDateTime::currentSecsSinceEpoch();
    // One row per call: a station that reconnected has two sids — the
    // freshest wins.
    QHash<QString, const Station*> best;
    for (const Station& st : stations_) {
        if (st.call.isEmpty() || st.hz < 1800000 || st.hz > 54000000) continue;
        const Station*& b = best[st.call];
        if (!b || std::max(st.lastUpdate, st.lastTx) > std::max(b->lastUpdate, b->lastTx)) b = &st;
    }
    QVector<Spot> v;
    for (const Station* st : std::as_const(best)) {
        Spot s;
        s.call = st->call;
        s.hz = st->hz;
        s.kind = 'V';
        s.tag = QStringLiteral("FreeDV");
        s.grid = st->grid;
        s.spotter = QStringLiteral("FreeDV Reporter");
        const QString mode = st->mode.isEmpty() ? QStringLiteral("FreeDV") : st->mode;
        const bool lately = st->lastTx > 0 && t - st->lastTx < kSpotLifeSecs;
        if (st->transmitting) {
            s.atSecs = t;
            s.comment = mode + " · ON AIR";
        } else if (st->lastTx > 0) {
            s.atSecs = st->lastTx;
            const qint64 m = (t - st->lastTx) / 60;
            s.comment = m < 1 ? mode + " · TX just now"
                      : m < 120 ? QString("%1 · TX %2 min ago").arg(mode).arg(m)
                                : QString("%1 · TX %2 h ago").arg(mode).arg(m / 60);
        } else {
            s.atSecs = st->lastUpdate > 0 ? st->lastUpdate : t;
            s.comment = mode + (st->rxOnly ? " · listening (RX only)" : " · listening");
        }
        if (!st->grid.isEmpty()) s.comment += " · " + st->grid;
        s.mapHidden = !(st->transmitting || lately);
        v.push_back(s);
    }
    for (const Spot& h : heard_)
        if (!best.contains(h.call)) v.push_back(h);
    return v;
}

} // namespace ttc
