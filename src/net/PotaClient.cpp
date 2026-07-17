// SPDX-License-Identifier: GPL-2.0-or-later
#include "net/PotaClient.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QTimeZone>
#include <cmath>

namespace ttc {

PotaClient::PotaClient(QObject* parent) : QObject(parent) {
    timer_.setInterval(60000);
    connect(&timer_, &QTimer::timeout, this, &PotaClient::poll);
}

void PotaClient::setEnabled(bool on) {
    if (on == enabled_) return;
    enabled_ = on;
    if (on) {
        timer_.start();
        poll();                                    // first batch right away
    } else {
        timer_.stop();
        if (!spots_.isEmpty()) {
            spots_.clear();
            emit spotsChanged();
        }
        emit statusChanged("POTA spots: off");
    }
}

void PotaClient::poll() {
    if (inFlight_) return;
    inFlight_ = true;
    QNetworkRequest req(QUrl("https://api.pota.app/spot/activator"));
    req.setTransferTimeout(15000);
    QNetworkReply* reply = nam_.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        inFlight_ = false;
        reply->deleteLater();
        if (!enabled_) return;
        if (reply->error() != QNetworkReply::NoError) {
            emit statusChanged("POTA spots: " + reply->errorString());
            return;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (!doc.isArray()) return;
        QVector<Spot> fresh;
        for (const QJsonValue& v : doc.array()) {
            const QJsonObject o = v.toObject();
            const QString call = o.value("activator").toString().toUpper();
            // frequency is kHz, sent as a string (occasionally fat-fingered:
            // the same HF/6m sanity clamp as the cluster parser).
            const double kHz = o.value("frequency").toString().toDouble();
            const qint64 hz = static_cast<qint64>(std::llround(kHz * 1000.0));
            if (call.isEmpty() || hz < 1800000 || hz > 54000000) continue;
            Spot s;
            s.call = call;
            s.hz   = hz;
            s.kind = 'P';
            s.tag  = o.value("reference").toString();
            s.lat  = o.value("latitude").toDouble(999.0);   // park coordinates
            s.lon  = o.value("longitude").toDouble(999.0);  // (bearing display)
            s.grid = o.value("grid6").toString();           // park locator —
            if (s.grid.isEmpty())                           // beats the
                s.grid = o.value("grid4").toString();       // home QTH grid
            // spotTime is ISO with no zone suffix but is UTC.
            QDateTime t = QDateTime::fromString(o.value("spotTime").toString(),
                                                Qt::ISODate);
            t.setTimeZone(QTimeZone::utc());
            s.atSecs = t.isValid() ? t.toSecsSinceEpoch()
                                   : QDateTime::currentSecsSinceEpoch();
            fresh.push_back(s);
        }
        spots_ = fresh;
        emit spotsChanged();
    });
}

} // namespace ttc
