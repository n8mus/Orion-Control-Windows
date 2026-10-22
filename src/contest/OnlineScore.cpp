// SPDX-License-Identifier: GPL-2.0-or-later
#include "contest/OnlineScore.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QUrl>

namespace ttc {

OnlineScore::OnlineScore(QObject* parent) : QObject(parent) {}

QString OnlineScore::url() {
    return QSettings().value("contest/cosUrl",
                             "https://contestonlinescore.com/post/").toString();
}

void OnlineScore::post(const QString& xml) {
    if (!nam_) nam_ = new QNetworkAccessManager(this);
    QNetworkRequest req{QUrl(url())};
    req.setHeader(QNetworkRequest::ContentTypeHeader, "text/xml");
    req.setHeader(QNetworkRequest::UserAgentHeader, "tentec-console");
    QNetworkReply* r = nam_->post(req, xml.toUtf8());
    connect(r, &QNetworkReply::finished, this, [this, r] {
        r->deleteLater();
        const int code = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool ok = r->error() == QNetworkReply::NoError && code >= 200 && code < 300;
        emit posted(ok, ok ? QStringLiteral("score posted")
                           : QString("score post failed: %1").arg(r->errorString()));
    });
}

} // namespace ttc
