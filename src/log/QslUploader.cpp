// SPDX-License-Identifier: GPL-2.0-or-later
#include "log/QslUploader.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>
#include <QUrl>

#include "log/LogDb.h"

namespace ttc {

namespace {
QString cfg(const QString& key, const QString& def = QString()) {
    return QSettings().value("up/" + key, def).toString().trimmed();
}
bool on(const QString& svc) {
    return QSettings().value("up/" + svc + "/enabled", false).toBool();
}
QString tqslDefault() {
#ifdef Q_OS_WIN
    return QStringLiteral(
        "C:/Program Files (x86)/TrustedQSL/tqsl.exe");
#else
    return QStringLiteral("tqsl");
#endif
}
// Query strings are built by hand — QUrlQuery re-encodes on output, which
// double-encodes anything passed through toPercentEncoding first.
QByteArray enc(const QString& s) {
    return QUrl::toPercentEncoding(s);
}
} // namespace

QslUploader::QslUploader(LogDb* db, QObject* parent)
    : QObject(parent), db_(db) {
    net_ = new QNetworkAccessManager(this);
    retry_ = new QTimer(this);
    retry_->setInterval(2 * 60 * 1000);   // catch-up sweep while running
    connect(retry_, &QTimer::timeout, this, [this] { sweep(); });
    retry_->start();
    sweepSoon(15000);                     // once after startup settles
}

void QslUploader::pushQso(qint64 id) {
    if (!db_ || id < 0) return;
    const Qso q = db_->qso(id);
    if (q.id < 0) return;
    for (const char* svc : {"eqsl", "qrz", "club", "hrdlog"})
        if (on(QLatin1String(svc))) pushHttp(q, QLatin1String(svc));
    if (on("lotw")) runTqslBatch();
    mirrorToLoggers(q);
}

void QslUploader::sweepSoon(int delayMs) {
    QTimer::singleShot(delayMs, this, [this] { sweep(); });
}

void QslUploader::sweep() {
    if (!db_) return;
    for (const char* svc : {"eqsl", "qrz", "club", "hrdlog"}) {
        if (!on(QLatin1String(svc))) continue;
        const auto pend = db_->pendingUploads(QLatin1String(svc), 10);
        for (const Qso& q : pend) pushHttp(q, QLatin1String(svc));
    }
    if (on("lotw")) runTqslBatch();
}

QString QslUploader::adifFor(const Qso& q, const QString& svc) const {
    AdifRecord r = LogDb::toAdif(q);
    // QSL-received flags describe MY log, not the outgoing report.
    r.remove("QSL_RCVD");
    r.remove("LOTW_QSL_RCVD");
    r.remove("EQSL_QSL_RCVD");
    if (svc == "qrz") {                 // the QRZ API rejects these fields
        r.remove("POTA_REF");
        r.remove("SIG");
        r.remove("SIG_INFO");
    }
    return Adif::writeRecord(r).trimmed();
}

void QslUploader::pushHttp(const Qso& q, const QString& svc) {
    const QString adif = adifFor(q, svc);
    const qint64 id = q.id;
    QNetworkReply* rep = nullptr;

    if (svc == "eqsl") {
        const QString user = cfg("eqsl/user"), pass = cfg("eqsl/password");
        if (user.isEmpty() || pass.isEmpty()) return;
        QByteArray qs = "ADIFData="
            + enc("<PROGRAMID:14>tentec-console <EOH>\r\n" + adif)
            + "&EQSL_USER=" + enc(user) + "&EQSL_PSWD=" + enc(pass);
        const QString nick = cfg("eqsl/nickname");
        if (!nick.isEmpty()) qs += "&EQSL_QTH_NICKNAME=" + enc(nick);
        QUrl url("https://www.eQSL.cc/qslcard/importADIF.cfm");
        url.setQuery(QString::fromLatin1(qs), QUrl::StrictMode);
        rep = net_->get(QNetworkRequest(url));
    } else if (svc == "qrz") {
        const QString key = cfg("qrz/key");
        if (key.isEmpty()) return;
        QNetworkRequest req(QUrl("https://logbook.qrz.com/api"));
        req.setHeader(QNetworkRequest::ContentTypeHeader,
                      "application/x-www-form-urlencoded");
        const QByteArray body =
            "KEY=" + enc(key) + "&ACTION=INSERT&ADIF=" + enc(adif);
        rep = net_->post(req, body);
    } else if (svc == "club") {
        const QString mail = cfg("club/email"), pass = cfg("club/password");
        const QString call = cfg("club/call"), api = cfg("club/apikey");
        if (mail.isEmpty() || pass.isEmpty() || call.isEmpty()
            || api.isEmpty())
            return;
        // The documented endpoint is secure.clublog.org (GridTracker posts
        // to clublog.org and rides the redirect; go direct instead — a
        // redirected POST can degrade to GET).
        QNetworkRequest req(
            QUrl("https://secure.clublog.org/realtime.php"));
        req.setHeader(QNetworkRequest::ContentTypeHeader,
                      "application/x-www-form-urlencoded");
        const QByteArray body = "email=" + enc(mail)
            + "&password=" + enc(pass) + "&callsign=" + enc(call)
            + "&api=" + enc(api) + "&adif=" + enc(adif);
        rep = net_->post(req, body);
    } else if (svc == "hrdlog") {
        const QString call = cfg("hrdlog/call"), code = cfg("hrdlog/code");
        if (call.isEmpty() || code.isEmpty()) return;
        QNetworkRequest req(QUrl("https://www.hrdlog.net/NewEntry.aspx"));
        req.setHeader(QNetworkRequest::ContentTypeHeader,
                      "application/x-www-form-urlencoded");
        const QByteArray body = "Callsign=" + enc(call) + "&Code="
            + enc(code) + "&App=tentec-console&ADIFData=" + enc(adif);
        rep = net_->post(req, body);
    }
    if (!rep) return;

    connect(rep, &QNetworkReply::finished, this, [this, rep, svc, id] {
        rep->deleteLater();
        const QString body = QString::fromUtf8(rep->readAll());
        bool ok = rep->error() == QNetworkReply::NoError;
        QString why = ok ? QString() : rep->errorString();
        if (ok) {
            if (svc == "eqsl") {
                if (body.contains("Duplicate", Qt::CaseInsensitive))
                    ok = true;                    // already there = delivered
                else if (body.contains("Error", Qt::CaseInsensitive)
                         || body.contains("No such",  Qt::CaseInsensitive)) {
                    ok = false;
                    why = body.contains("Username", Qt::CaseInsensitive)
                        ? "eQSL rejected the credentials"
                        : "eQSL rejected the record";
                }
            } else if (svc == "qrz") {
                if (body.contains("STATUS=FAIL", Qt::CaseInsensitive)) {
                    if (body.contains("duplicate", Qt::CaseInsensitive))
                        ok = true;
                    else {
                        ok = false;
                        why = "QRZ: " + body.left(120);
                    }
                }
            }
            // club/hrdlog: HTTP success is the acknowledgment we get.
        }
        db_->setUploadState(id, svc, ok ? QChar('Y') : QChar('E'));
        if (!ok) emit serviceResult(svc, false, why);
    });
}

void QslUploader::runTqslBatch() {
    if (tqslRunning_ || !db_) return;
    const QString station = cfg("lotw/station");
    const QString tqsl = cfg("lotw/tqslPath", tqslDefault());
    if (station.isEmpty()) return;
    const auto pend = db_->pendingUploads("lotw", 100);
    if (pend.isEmpty()) return;

    QString text = "<PROGRAMID:14>tentec-console <EOH>\n";
    QList<qint64> ids;
    for (const Qso& q : pend) {
        text += Adif::writeRecord(LogDb::toAdif(q));
        ids.push_back(q.id);
    }
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    const QString path = dir + QString("/ttc-lotw-%1.adi")
                                   .arg(QCoreApplication::applicationPid());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    f.write(text.toUtf8());
    f.close();

    QStringList args{"-a", "all", "-l", station};
    const QString pw = cfg("lotw/tqslPassword");
    if (!pw.isEmpty()) { args << "-p" << pw; }
    args << "-q" << "-x" << "-d" << "-u" << path;

    tqslRunning_ = true;
    auto* p = new QProcess(this);
    connect(p, &QProcess::finished, this,
            [this, p, path, ids](int code, QProcess::ExitStatus st) {
        const QString err = QString::fromUtf8(p->readAllStandardError());
        p->deleteLater();
        tqslRunning_ = false;
        QFile::remove(path);
        const bool ok = st == QProcess::NormalExit
            && (err.contains("Final Status: Success")
                || err.contains("duplicate", Qt::CaseInsensitive)
                || code == 0);
        for (const qint64 id : ids)
            db_->setUploadState(id, "lotw", ok ? QChar('Y') : QChar('E'));
        if (!ok)
            emit serviceResult(
                "lotw", false,
                err.isEmpty() ? QString("tqsl exit %1").arg(code)
                              : err.right(160).trimmed());
    });
    connect(p, &QProcess::errorOccurred, this,
            [this, p, path, ids](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;   // finished handles others
        p->deleteLater();
        tqslRunning_ = false;
        QFile::remove(path);
        for (const qint64 id : ids) db_->setUploadState(id, "lotw", 'E');
        emit serviceResult("lotw", false, "can't run tqsl — check the path");
    });
    p->start(tqsl, args);
    QTimer::singleShot(60000, p, [p] {
        if (p->state() != QProcess::NotRunning) p->kill();
    });
}

void QslUploader::mirrorToLoggers(const Qso& q) {
    // N1MM+: a raw ADIF record as one UDP datagram (WSJT-X convention).
    if (on("n1mm")) {
        const QString ip = cfg("n1mm/ip", "127.0.0.1");
        const quint16 port =
            quint16(QSettings().value("up/n1mm/port", 2333).toUInt());
        QUdpSocket s;
        s.writeDatagram(adifFor(q, "n1mm").toUtf8(), QHostAddress(ip), port);
    }
    // HRD Logbook: its TCP command console ("db add {F=\"v\" ...}").
    if (on("hrd")) {
        const QString ip = cfg("hrd/ip", "127.0.0.1");
        const quint16 port =
            quint16(QSettings().value("up/hrd/port", 7826).toUInt());
        AdifRecord r = LogDb::toAdif(q);
        // HRD wants the frequency without the decimal point (Hz digits).
        if (r.contains("FREQ"))
            r["FREQ"] = r["FREQ"].remove('.');
        QString cmd = "ver\rdb add {";
        for (auto it = r.constBegin(); it != r.constEnd(); ++it)
            cmd += it.key() + "=\"" + it.value() + "\" ";
        cmd += "}\rexit\r";
        auto* sock = new QTcpSocket(this);
        connect(sock, &QTcpSocket::connected, this, [sock, cmd] {
            sock->write(cmd.toUtf8());
            sock->flush();
            sock->disconnectFromHost();
        });
        connect(sock, &QTcpSocket::disconnected, sock,
                &QObject::deleteLater);
        connect(sock, &QTcpSocket::errorOccurred, this,
                [this, sock](QAbstractSocket::SocketError) {
            emit serviceResult("hrd", false,
                               "HRD Logbook not reachable: "
                                   + sock->errorString());
            sock->deleteLater();
        });
        sock->connectToHost(ip, port);
    }
}

// ---- Setup-window tests ----------------------------------------------

void QslUploader::testLotwDownload() {
    const QString user = cfg("lotw/login"), pass = cfg("lotw/password");
    if (user.isEmpty() || pass.isEmpty()) {
        emit serviceResult("lotw-dl", false, "enter LoTW login + password");
        return;
    }
    QUrl url("https://lotw.arrl.org/lotwuser/lotwreport.adi");
    url.setQuery(QString::fromLatin1(
                     "login=" + enc(user) + "&password=" + enc(pass)
                     + "&qso_query=1&qso_qsosince=2100-01-01"),
                 QUrl::StrictMode);                  // auth check, no data
    QNetworkReply* rep = net_->get(QNetworkRequest(url));
    connect(rep, &QNetworkReply::finished, this, [this, rep] {
        rep->deleteLater();
        const QString body = QString::fromUtf8(rep->readAll());
        if (rep->error() != QNetworkReply::NoError)
            emit serviceResult("lotw-dl", false, rep->errorString());
        else if (body.contains("password incorrect", Qt::CaseInsensitive)
                 || body.contains("Username/password", Qt::CaseInsensitive))
            emit serviceResult("lotw-dl", false, "LoTW rejected the login");
        else
            emit serviceResult("lotw-dl", true, "logged in OK");
    });
}

void QslUploader::testTqsl() {
    const QString tqsl = cfg("lotw/tqslPath", tqslDefault());
    auto* p = new QProcess(this);
    connect(p, &QProcess::finished, this,
            [this, p](int code, QProcess::ExitStatus st) {
        const QString err =
            QString::fromUtf8(p->readAllStandardError()).trimmed();
        p->deleteLater();
        if (st != QProcess::NormalExit || code != 0)
            emit serviceResult("lotw-tqsl", false,
                               QString("tqsl exit %1").arg(code));
        else {
            QString d = err.left(80);
            const QString pw = cfg("lotw/tqslPassword");
            d += pw.isEmpty() ? " · no key password (correct for this"
                                " station's certificates)"
                              : " · key password set";
            emit serviceResult("lotw-tqsl", true, d);
        }
    });
    connect(p, &QProcess::errorOccurred, this,
            [this, p](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;
        emit serviceResult("lotw-tqsl", false,
                           "can't run tqsl — check the path");
        p->deleteLater();
    });
    p->start(tqsl, {"-q", "-v"});
}

void QslUploader::testEqsl() {
    const QString user = cfg("eqsl/user"), pass = cfg("eqsl/password");
    if (user.isEmpty() || pass.isEmpty()) {
        emit serviceResult("eqsl", false, "enter eQSL user + password");
        return;
    }
    QUrl url("https://www.eQSL.cc/qslcard/DownloadInBox.cfm");
    url.setQuery(QString::fromLatin1("UserName=" + enc(user) + "&Password="
                                     + enc(pass) + "&RcvdSince=21000101"),
                 QUrl::StrictMode);
    QNetworkReply* rep = net_->get(QNetworkRequest(url));
    connect(rep, &QNetworkReply::finished, this, [this, rep] {
        rep->deleteLater();
        const QString body = QString::fromUtf8(rep->readAll());
        if (rep->error() != QNetworkReply::NoError)
            emit serviceResult("eqsl", false, rep->errorString());
        else if (body.contains("No such Username", Qt::CaseInsensitive)
                 || body.contains("incorrect password",
                                  Qt::CaseInsensitive))
            emit serviceResult("eqsl", false, "eQSL rejected the login");
        else
            emit serviceResult("eqsl", true, "logged in OK");
    });
}

void QslUploader::testQrz() {
    const QString key = cfg("qrz/key");
    if (key.isEmpty()) {
        emit serviceResult("qrz", false, "enter the logbook API key");
        return;
    }
    QNetworkRequest req(QUrl("https://logbook.qrz.com/api"));
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  "application/x-www-form-urlencoded");
    QNetworkReply* rep =
        net_->post(req, "KEY=" + enc(key) + "&ACTION=STATUS");
    connect(rep, &QNetworkReply::finished, this, [this, rep] {
        rep->deleteLater();
        const QString body = QString::fromUtf8(rep->readAll());
        if (rep->error() != QNetworkReply::NoError)
            emit serviceResult("qrz", false, rep->errorString());
        else if (body.contains("RESULT=OK", Qt::CaseInsensitive)) {
            QString n;
            for (const QString& kv : body.split('&'))
                if (kv.startsWith("COUNT=")) n = kv.mid(6);
            emit serviceResult("qrz", true,
                               n.isEmpty() ? "key OK"
                                           : n + " records in the logbook");
        } else
            emit serviceResult("qrz", false,
                               "QRZ rejected the key: " + body.left(100));
    });
}

void QslUploader::testHrdlogNet() {
    // hrdlog.net has no cheap auth probe that doesn't insert a QSO; the
    // honest test is field presence (the first real upload proves it).
    const QString call = cfg("hrdlog/call"), code = cfg("hrdlog/code");
    if (call.isEmpty() || code.isEmpty())
        emit serviceResult("hrdlog", false, "enter callsign + upload code");
    else
        emit serviceResult("hrdlog", true,
                           "fields set — verified on first upload");
}

void QslUploader::testLoggerPush(const QString& svc) {
    const QString ip = cfg(svc + "/ip", "127.0.0.1");
    const quint16 port = quint16(QSettings()
        .value("up/" + svc + "/port", svc == "hrd" ? 7826 : 2333).toUInt());
    auto* sock = new QTcpSocket(this);
    connect(sock, &QTcpSocket::connected, this, [this, sock, svc] {
        emit serviceResult(svc, true, "reachable");
        sock->disconnectFromHost();
        sock->deleteLater();
    });
    connect(sock, &QTcpSocket::errorOccurred, this,
            [this, sock, svc](QAbstractSocket::SocketError) {
        emit serviceResult(svc, false,
                           svc == "n1mm"
                               ? "nothing listening (UDP is fire-and-forget;"
                                 " this checks a TCP listener)"
                               : "not reachable: " + sock->errorString());
        sock->deleteLater();
    });
    sock->connectToHost(ip, port);
}

} // namespace ttc
