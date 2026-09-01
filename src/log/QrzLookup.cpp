// SPDX-License-Identifier: GPL-2.0-or-later
#include "log/QrzLookup.h"

#include <QHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QUrl>
#include <QXmlStreamReader>

namespace ttc {

namespace {
QString cred(const char* k) {
    return QSettings().value(QLatin1String(k)).toString().trimmed();
}
// Leaf-element text out of the QRZ XML ("Key", "fname", ...). The
// document nests (<QRZDatabase><Session><Key>…), so walk tokens and take
// the text under whatever element we're inside — readElementText on a
// parent consumes its whole subtree and returns nothing (live-found:
// every lookup died silently on the invisible session key).
QHash<QString, QString> flatXml(const QByteArray& body) {
    QHash<QString, QString> out;
    QXmlStreamReader xml(body);
    QString cur;
    while (!xml.atEnd()) {
        switch (xml.readNext()) {
            case QXmlStreamReader::StartElement:
                cur = xml.name().toString();
                break;
            case QXmlStreamReader::Characters:
                if (!xml.isWhitespace() && !cur.isEmpty())
                    out.insert(cur, xml.text().toString().trimmed());
                break;
            default:
                break;
        }
    }
    return out;
}
} // namespace

QrzLookup::QrzLookup(QObject* parent) : QObject(parent) {
    net_ = new QNetworkAccessManager(this);
}

void QrzLookup::lookup(const QString& call) {
    const QString c = call.trimmed().toUpper();
    if (c.isEmpty()) return;
    if (cred("up/qrzweb/user").isEmpty()
        || cred("up/qrzweb/password").isEmpty()) {
        emit result(c, false, {}, {}, {},
                    "enter the QRZ website login under Online Logs");
        return;
    }
    if (key_.isEmpty())
        fetchKey(c);
    else
        query(c, true);
}

void QrzLookup::fetchKey(const QString& thenCall) {
    QUrl url("https://xmldata.qrz.com/xml/current/");
    url.setQuery(QString::fromLatin1(
        "username=" + QUrl::toPercentEncoding(cred("up/qrzweb/user"))
        + "&password="
        + QUrl::toPercentEncoding(cred("up/qrzweb/password"))
        + "&agent=tentec-console"), QUrl::StrictMode);
    QNetworkReply* rep = net_->get(QNetworkRequest(url));
    connect(rep, &QNetworkReply::finished, this, [this, rep, thenCall] {
        rep->deleteLater();
        if (rep->error() != QNetworkReply::NoError) {
            emit result(thenCall, false, {}, {}, {}, rep->errorString());
            return;
        }
        const auto xml = flatXml(rep->readAll());
        key_ = xml.value("Key");
        if (key_.isEmpty()) {
            emit result(thenCall, false, {}, {}, {},
                        xml.value("Error", "QRZ rejected the login"));
            return;
        }
        query(thenCall, false);
    });
}

void QrzLookup::query(const QString& call, bool retryOnBadKey) {
    QUrl url("https://xmldata.qrz.com/xml/current/");
    url.setQuery(QString::fromLatin1(
        "s=" + QUrl::toPercentEncoding(key_) + "&callsign="
        + QUrl::toPercentEncoding(call)), QUrl::StrictMode);
    QNetworkReply* rep = net_->get(QNetworkRequest(url));
    connect(rep, &QNetworkReply::finished, this,
            [this, rep, call, retryOnBadKey] {
        rep->deleteLater();
        if (rep->error() != QNetworkReply::NoError) {
            emit result(call, false, {}, {}, {}, rep->errorString());
            return;
        }
        const auto xml = flatXml(rep->readAll());
        const QString err = xml.value("Error");
        if (!err.isEmpty()) {
            // "Session Timeout" / "Invalid session key" -> one refresh.
            if (retryOnBadKey
                && err.contains("session", Qt::CaseInsensitive)) {
                key_.clear();
                fetchKey(call);
                return;
            }
            emit result(call, false, {}, {}, {}, err);
            return;
        }
        QString name = (xml.value("fname") + " " + xml.value("name"))
                           .trimmed();
        if (name.isEmpty()) name = xml.value("name");
        QString qth = xml.value("addr2");
        const QString state = xml.value("state");
        if (!state.isEmpty()) qth += (qth.isEmpty() ? "" : ", ") + state;
        emit result(call, true, name, qth, xml.value("grid").toUpper(),
                    QString());
    });
}

} // namespace ttc
