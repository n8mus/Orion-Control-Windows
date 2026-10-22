// SPDX-License-Identifier: GPL-2.0-or-later
#include "net/FldigiClient.h"

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QXmlStreamReader>

namespace ttc {

FldigiClient::FldigiClient(QObject* parent) : QObject(parent) {
    timer_ = new QTimer(this);
    timer_->setInterval(500);
    connect(timer_, &QTimer::timeout, this, &FldigiClient::poll);
}

void FldigiClient::setEndpoint(const QString& host, quint16 port) {
    host_ = host;
    port_ = port;
}

void FldigiClient::setActive(bool on) {
    if (on == timer_->isActive()) return;
    if (on) {
        rxLen_ = -1;                       // fresh session: skip old text
        timer_->start();
        poll();
    } else {
        timer_->stop();
        setConnected(false);
    }
}

void FldigiClient::setConnected(bool on) {
    if (on == connected_) return;
    connected_ = on;
    if (!on) {
        modem_.clear();
        carrier_ = 0;
        rxLen_ = -1;
    }
    emit statusChanged(connected_, modem_, carrier_);
}

void FldigiClient::setCarrier(int audioHz) {
    call(QStringLiteral("modem.set_carrier"), {audioHz},
         [this](bool ok, const QString&) {
             if (ok) poll();               // reflect the move right away
         });
}

void FldigiClient::call(const QString& method, const QList<QVariant>& args,
                        std::function<void(bool, const QString&)> done) {
    QString body = QStringLiteral(
        "<?xml version=\"1.0\"?><methodCall><methodName>%1</methodName>"
        "<params>").arg(method);
    for (const QVariant& a : args) {
        if (a.typeId() == QMetaType::Int)
            body += QStringLiteral("<param><value><int>%1</int></value>"
                                   "</param>").arg(a.toInt());
        else
            body += QStringLiteral("<param><value><string>%1</string>"
                                   "</value></param>")
                        .arg(a.toString().toHtmlEscaped());
    }
    body += QStringLiteral("</params></methodCall>");
    QNetworkRequest req(
        QUrl(QStringLiteral("http://%1:%2/RPC2").arg(host_).arg(port_)));
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("text/xml"));
    req.setTransferTimeout(1500);
    QNetworkReply* rep = nam_.post(req, body.toUtf8());
    ++inFlight_;
    connect(rep, &QNetworkReply::finished, this,
            [this, rep, done = std::move(done)] {
                --inFlight_;
                rep->deleteLater();
                if (rep->error() != QNetworkReply::NoError) {
                    done(false, QString());
                    return;
                }
                // Flatten the first <value>: <base64> decodes, any other
                // type (or bare text) passes through as a string.
                QXmlStreamReader xml(rep->readAll());
                QString val;
                bool b64 = false, inValue = false;
                while (!xml.atEnd()) {
                    xml.readNext();
                    if (xml.isStartElement()) {
                        if (xml.name() == QLatin1String("value"))
                            inValue = true;
                        else if (inValue
                                 && xml.name() == QLatin1String("base64"))
                            b64 = true;
                    } else if (inValue && xml.isCharacters()
                               && !xml.isWhitespace()) {
                        val += xml.text();
                    } else if (xml.isEndElement()
                               && xml.name() == QLatin1String("value")) {
                        break;
                    }
                }
                if (b64)
                    val = QString::fromUtf8(
                        QByteArray::fromBase64(val.toLatin1()));
                done(true, val);
            });
}

void FldigiClient::poll() {
    if (inFlight_ > 2) return;             // dead link: let timeouts drain
    call(QStringLiteral("modem.get_name"), {},
         [this](bool ok, const QString& v) {
             if (!ok) { setConnected(false); return; }
             const bool announce = !connected_ || v != modem_;
             connected_ = true;
             modem_ = v;
             if (announce) emit statusChanged(true, modem_, carrier_);
         });
    call(QStringLiteral("modem.get_carrier"), {},
         [this](bool ok, const QString& v) {
             if (!ok || !connected_) return;
             const int c = v.toInt();
             if (c != carrier_) {
                 carrier_ = c;
                 emit statusChanged(true, modem_, carrier_);
             }
         });
    call(QStringLiteral("text.get_rx_length"), {},
         [this](bool ok, const QString& v) {
             if (!ok) return;
             const qint64 len = v.toLongLong();
             if (rxLen_ < 0 || len < rxLen_)
                 rxLen_ = len;             // first contact / buffer cleared
             if (len <= rxLen_) return;
             const qint64 start = rxLen_;
             const qint64 want = std::min<qint64>(len - rxLen_, 512);
             rxLen_ = start + want;
             call(QStringLiteral("text.get_rx"),
                  {int(start), int(want)},
                  [this](bool ok2, const QString& text) {
                      if (ok2 && !text.isEmpty()) emit rxText(text);
                  });
         });
}

} // namespace ttc
