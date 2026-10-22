// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QObject>
#include <QString>

class QNetworkAccessManager;

namespace ttc {

// Posts the running score to contestonlinescore.com (the scoreboard N1MM
// and friends feed) — opt-in per contest from the deck, never automatic.
// The XML itself is ContestEngine's onlineScoreXml(); this only ships it.
class OnlineScore : public QObject {
    Q_OBJECT
public:
    explicit OnlineScore(QObject* parent = nullptr);
    void post(const QString& xml);
    static QString url();                // contest/cosUrl, a default otherwise

signals:
    void posted(bool ok, const QString& detail);

private:
    QNetworkAccessManager* nam_ = nullptr;
};

} // namespace ttc
