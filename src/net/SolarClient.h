// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QNetworkAccessManager>
#include <QObject>
#include <QTimer>
#include <functional>

namespace ttc {

// Space-weather snapshot for the display layer.
struct SolarData {
    int sfi  = -1;      // 10.7 cm solar flux
    int aIdx = -1;      // planetary A-index
    int ssn  = -1;      // sunspot number
    double kIdx = -1.0; // planetary K-index (estimated, from WWV)
    QString xray;       // GOES long-band class, e.g. "B5.9"
    bool valid() const { return sfi > 0; }
};

// NOAA SWPC poller for the Thetis-style solar panel + map sun marker.
// Three small public endpoints, refreshed every 15 minutes while enabled:
//   text/wwv.txt                       -> SFI, A-index, K-index (one file)
//   text/daily-solar-indices.txt       -> sunspot number (last data row)
//   json/goes/primary/xrays-6-hour.json -> latest 0.1-0.8 nm flux -> class
// Failures keep the last good values; nothing here ever blocks the UI.
class SolarClient : public QObject {
    Q_OBJECT
public:
    explicit SolarClient(QObject* parent = nullptr);

    void setEnabled(bool on);
    bool enabled() const { return enabled_; }
    SolarData data() const { return d_; }

signals:
    void updated();

private:
    void poll();
    void fetch(const QString& url,
               std::function<void(const QByteArray&)> parse);

    QNetworkAccessManager nam_;
    QTimer timer_;
    SolarData d_;
    bool enabled_ = false;
};

} // namespace ttc
