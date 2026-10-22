// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QNetworkAccessManager>
#include <QObject>
#include <QTimer>
#include <QVector>

#include "net/SpotClient.h"   // Spot

namespace ttc {

// POTA activator feed (same API the cqrlog fork's POTA cluster uses):
// polls https://api.pota.app/spot/activator once a minute and turns the JSON
// into Spots (kind 'P', tag = park reference like "US-2654"). The API always
// returns the complete current-activations list, so each poll replaces the
// previous set outright — no TTL bookkeeping.
class PotaClient : public QObject {
    Q_OBJECT
public:
    explicit PotaClient(QObject* parent = nullptr);

    void setEnabled(bool on);
    bool enabled() const { return enabled_; }
    QVector<Spot> spots() const { return spots_; }

signals:
    void spotsChanged();
    void statusChanged(const QString& s);

private:
    void poll();

    QNetworkAccessManager nam_;
    QTimer timer_;
    QVector<Spot> spots_;
    bool enabled_ = false;
    bool inFlight_ = false;
};

} // namespace ttc
