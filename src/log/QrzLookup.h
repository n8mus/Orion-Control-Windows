// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QObject>
#include <QString>

class QNetworkAccessManager;

namespace ttc {

// QRZ.com XML callbook lookup (xmldata.qrz.com): name/QTH/grid for the LOG
// window's QRZ button. Uses the QRZ WEBSITE login (up/qrzweb/user +
// up/qrzweb/password — the XML service rides the operator's QRZ
// subscription and is a different credential from the logbook API key).
// Session key is fetched on first use and refreshed once when it expires.
class QrzLookup : public QObject {
    Q_OBJECT
public:
    explicit QrzLookup(QObject* parent = nullptr);
    void lookup(const QString& call);

signals:
    // ok=false: error carries the reason (bad login, not found, offline).
    void result(const QString& call, bool ok, const QString& name,
                const QString& qth, const QString& grid,
                const QString& error);

private:
    void fetchKey(const QString& thenCall);
    void query(const QString& call, bool retryOnBadKey);

    QNetworkAccessManager* net_;
    QString key_;
};

} // namespace ttc
