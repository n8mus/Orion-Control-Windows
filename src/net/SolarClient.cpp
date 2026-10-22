// SPDX-License-Identifier: GPL-2.0-or-later
#include "net/SolarClient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QRegularExpression>

namespace ttc {

SolarClient::SolarClient(QObject* parent) : QObject(parent) {
    timer_.setInterval(15 * 60 * 1000);
    connect(&timer_, &QTimer::timeout, this, &SolarClient::poll);
}

void SolarClient::setEnabled(bool on) {
    if (on == enabled_) return;
    enabled_ = on;
    if (on) {
        timer_.start();
        poll();                                    // first values right away
    } else {
        timer_.stop();
    }
}

void SolarClient::fetch(const QString& url,
                        std::function<void(const QByteArray&)> parse) {
    QNetworkRequest req{QUrl(url)};
    req.setTransferTimeout(15000);
    QNetworkReply* reply = nam_.get(req);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, parse = std::move(parse)] {
        reply->deleteLater();
        if (!enabled_ || reply->error() != QNetworkReply::NoError) return;
        parse(reply->readAll());
        emit updated();
    });
}

void SolarClient::poll() {
    // WWV announcement: "Solar flux 112 and estimated planetary A-index 18.
    // The estimated planetary K-index at 1800 UTC on 12 July was 3.33."
    fetch("https://services.swpc.noaa.gov/text/wwv.txt",
          [this](const QByteArray& body) {
        const QString t = QString::fromLatin1(body);
        static const QRegularExpression fluxRe(
            R"(Solar flux (\d+) and estimated planetary A-index (\d+))");
        static const QRegularExpression kRe(
            R"(K-index at \d+ UTC on .+ was ([\d.]+))");
        if (const auto m = fluxRe.match(t); m.hasMatch()) {
            d_.sfi  = m.captured(1).toInt();
            d_.aIdx = m.captured(2).toInt();
        }
        if (const auto m = kRe.match(t); m.hasMatch())
            d_.kIdx = m.captured(1).toDouble();
    });
    // Daily indices: last data row is "yyyy mm dd  flux  SSN  area ...".
    fetch("https://services.swpc.noaa.gov/text/daily-solar-indices.txt",
          [this](const QByteArray& body) {
        const QStringList lines = QString::fromLatin1(body).split('\n');
        for (int i = lines.size() - 1; i >= 0; --i) {
            const QStringList f =
                lines[i].simplified().split(' ', Qt::SkipEmptyParts);
            if (f.size() >= 5 && f[0].size() == 4 && f[0][0].isDigit()) {
                d_.ssn = f[4].toInt();
                break;
            }
        }
    });
    // GOES X-ray: newest long-band (0.1-0.8 nm) flux -> letter class.
    fetch("https://services.swpc.noaa.gov/json/goes/primary/xrays-6-hour.json",
          [this](const QByteArray& body) {
        const QJsonDocument doc = QJsonDocument::fromJson(body);
        if (!doc.isArray()) return;
        double flux = -1.0;
        const QJsonArray arr = doc.array();
        for (int i = arr.size() - 1; i >= 0; --i) {   // newest is at the end
            const QJsonObject o = arr[i].toObject();
            if (o.value("energy").toString() == "0.1-0.8nm") {
                flux = o.value("flux").toDouble(-1.0);
                break;
            }
        }
        if (flux <= 0.0) return;
        struct Band { double lo; char letter; };
        static const Band bands[] = {
            {1e-4, 'X'}, {1e-5, 'M'}, {1e-6, 'C'}, {1e-7, 'B'}, {0.0, 'A'},
        };
        for (const Band& b : bands)
            if (flux >= b.lo) {
                const double mant = b.lo > 0.0 ? flux / b.lo : flux / 1e-8;
                d_.xray = QString("%1%2").arg(b.letter)
                              .arg(mant, 0, 'f', mant < 10 ? 1 : 0);
                break;
            }
    });
}

} // namespace ttc
