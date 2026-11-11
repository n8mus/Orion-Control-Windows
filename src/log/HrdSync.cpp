// SPDX-License-Identifier: GPL-2.0-or-later
#include "log/HrdSync.h"

#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QHostAddress>
#include <QPointer>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QThread>
#include <QTimeZone>
#include <QUuid>

#include <cstdlib>

#include "log/Adif.h"
#include "log/LogDb.h"

namespace ttc {

namespace {

// Open HRD's Access file read-only on a private connection. The caller
// owns the name and must removeDatabase() it after the handle is gone.
bool openMdb(const QString& conn, QString* err) {
    QSqlDatabase db = QSqlDatabase::addDatabase("QODBC", conn);
    db.setDatabaseName(
        QString("DRIVER={Microsoft Access Driver (*.mdb, *.accdb)};"
                "DBQ=%1;ReadOnly=1;")
            .arg(QDir::toNativeSeparators(HrdSync::mdbPath())));
    db.setConnectOptions("SQL_ATTR_ACCESS_MODE=SQL_MODE_READ_ONLY");
    if (db.open()) return true;
    if (err) *err = db.lastError().text();
    return false;
}

// Access hands DATETIME back as a zone-less local QDateTime; HRD stores
// UTC wall time in it, so keep the digits and call them UTC.
QDateTime utcOf(const QVariant& v) {
    const QDateTime t = v.toDateTime();
    if (!t.isValid()) return {};
    return QDateTime(t.date(), t.time(), QTimeZone::utc());
}

QString str(const QSqlQuery& q, int i) {
    return q.value(i).isNull() ? QString() : q.value(i).toString().trimmed();
}

const char* kPullCols =
    "COL_PRIMARY_KEY, COL_TIME_ON, COL_CALL, COL_FREQ, COL_BAND, COL_MODE,"
    " COL_SUBMODE, COL_RST_SENT, COL_RST_RCVD, COL_NAME, COL_QTH,"
    " COL_GRIDSQUARE, COL_COUNTRY, COL_CQZ, COL_ITUZ, COL_QSL_RCVD,"
    " COL_LOTW_QSL_RCVD, COL_EQSL_QSL_RCVD, COL_SIG, COL_SIG_INFO,"
    " COL_COMMENT";

// One pull: HRD rows past the watermark as ADIF.
struct PullResult {
    QByteArray adif;
    qint64 maxKey = 0;
    int rows = 0;
    QString err;
};

PullResult readNewRows(qint64 afterKey) {
    PullResult out;
    out.maxKey = afterKey;
    const QString conn = "hrd-pull-" + QUuid::createUuid().toString();
    {
        if (!openMdb(conn, &out.err)) {
            QSqlDatabase::removeDatabase(conn);
            return out;
        }
        QSqlQuery q(QSqlDatabase::database(conn));
        q.setForwardOnly(true);
        if (!q.exec(QString("SELECT %1 FROM TABLE_HRD_CONTACTS_V01"
                            " WHERE COL_PRIMARY_KEY > %2"
                            " ORDER BY COL_PRIMARY_KEY")
                        .arg(QLatin1String(kPullCols))
                        .arg(afterKey))) {
            out.err = q.lastError().text();
        } else {
            while (q.next()) {
                ++out.rows;
                const qint64 key = q.value(0).toLongLong();
                if (key > out.maxKey) out.maxKey = key;
                const QDateTime ts = utcOf(q.value(1));
                const QString call = str(q, 2).toUpper();
                if (!ts.isValid() || call.isEmpty()) continue;
                AdifRecord r;
                r.insert("CALL", call);
                r.insert("QSO_DATE", ts.toString("yyyyMMdd"));
                r.insert("TIME_ON", ts.toString("HHmmss"));
                const qint64 hz = q.value(3).toLongLong();   // HRD: Hz
                if (hz > 0) r.insert("FREQ", QString::number(hz / 1e6, 'f', 6));
                const auto put = [&](const char* k, int i) {
                    const QString v = str(q, i);
                    if (!v.isEmpty()) r.insert(QLatin1String(k), v);
                };
                put("BAND", 4);
                put("MODE", 5);
                put("SUBMODE", 6);
                put("RST_SENT", 7);
                put("RST_RCVD", 8);
                put("NAME", 9);
                put("QTH", 10);
                put("GRIDSQUARE", 11);
                put("COUNTRY", 12);
                if (q.value(13).toInt() > 0) put("CQZ", 13);
                if (q.value(14).toInt() > 0) put("ITUZ", 14);
                put("QSL_RCVD", 15);
                put("LOTW_QSL_RCVD", 16);
                put("EQSL_QSL_RCVD", 17);
                put("SIG", 18);
                put("SIG_INFO", 19);
                put("COMMENT", 20);
                out.adif += Adif::writeRecord(r).toUtf8();
            }
        }
    }
    QSqlDatabase::removeDatabase(conn);
    return out;
}

// HRD's whole log as CALL|BAND -> epoch seconds (the compare side).
QHash<QString, QList<qint64>> readAllKeys(QString* err) {
    QHash<QString, QList<qint64>> have;
    const QString conn = "hrd-check-" + QUuid::createUuid().toString();
    {
        if (!openMdb(conn, err)) {
            QSqlDatabase::removeDatabase(conn);
            return have;
        }
        QSqlQuery q(QSqlDatabase::database(conn));
        q.setForwardOnly(true);
        if (!q.exec("SELECT COL_CALL, COL_BAND, COL_TIME_ON"
                    " FROM TABLE_HRD_CONTACTS_V01")) {
            if (err) *err = q.lastError().text();
        } else {
            while (q.next()) {
                const QDateTime ts = utcOf(q.value(2));
                if (!ts.isValid()) continue;
                have[str(q, 0).toUpper() + '|' + str(q, 1).toUpper()]
                    << ts.toSecsSinceEpoch();
            }
        }
    }
    QSqlDatabase::removeDatabase(conn);
    return have;
}

quint16 hrdPort() {
    return quint16(QSettings().value("up/hrd/port", 7826).toUInt());
}
QString hrdHost() {
    return QSettings().value("up/hrd/ip", "127.0.0.1").toString();
}

} // namespace

HrdSync::HrdSync(LogDb* db, const CtyLookup* cty, QObject* parent)
    : QObject(parent), db_(db), cty_(cty) {
    lastKey_ = QSettings().value("log/hrdSyncKey", 0).toLongLong();
    timer_.setInterval(2 * 60 * 1000);
    connect(&timer_, &QTimer::timeout, this, [this] { pull(); });
}

QString HrdSync::mdbPath() {
    const QString set = QSettings().value("log/hrdMdb").toString();
    if (!set.isEmpty()) return set;
    // %APPDATA%\HRDLLC\HRD Logbook\HRD My Logbook.mdb — HRD's default.
    const QString roaming = QDir::fromNativeSeparators(
        QString::fromLocal8Bit(qgetenv("APPDATA")));
    return roaming + "/HRDLLC/HRD Logbook/HRD My Logbook.mdb";
}

void HrdSync::start() {
    if (!QSettings().value("log/hrdSync", true).toBool()) return;
    if (!QFileInfo::exists(mdbPath())) return;   // no HRD on this PC
    timer_.start();
    pullSoon(8000);                              // let startup settle first
}

void HrdSync::pullSoon(int delayMs) {
    QTimer::singleShot(delayMs, this, [this] { pull(); });
}

void HrdSync::pull() {
    if (busy_ || !db_) return;
    // Cheap gate: nothing to read unless HRD's file changed since we last
    // looked (HRD writes it on every saved contact).
    const QFileInfo fi(mdbPath());
    if (!fi.exists()) return;
    static QDateTime lastSeen;
    if (lastKey_ > 0 && fi.lastModified() == lastSeen) return;
    lastSeen = fi.lastModified();

    busy_ = true;
    const qint64 after = lastKey_;
    QPointer<HrdSync> self(this);
    QThread* t = QThread::create([self, after] {
        const PullResult r = readNewRows(after);
        if (!self) return;
        QMetaObject::invokeMethod(self.data(), [self, r] {
            if (!self) return;
            self->busy_ = false;
            if (!r.err.isEmpty()) {
                std::fprintf(stderr, "[hrd] can't read HRD's log: %s\n",
                             qPrintable(r.err));
                return;
            }
            if (r.maxKey <= self->lastKey_) return;
            int added = 0;
            if (!r.adif.isEmpty()) {
                QBuffer buf;
                buf.setData(r.adif);
                buf.open(QIODevice::ReadOnly);
                added = self->db_->importAdif(buf, self->cty_);
            }
            self->lastKey_ = r.maxKey;
            QSettings().setValue("log/hrdSyncKey", self->lastKey_);
            std::fprintf(stderr, "[hrd] read %d HRD row(s), %d new to the "
                         "station log\n", r.rows, added);
            if (added > 0) emit self->pulled(added);
        }, Qt::QueuedConnection);
    });
    connect(t, &QThread::finished, t, &QObject::deleteLater);
    t->start(QThread::LowPriority);
}

void HrdSync::checkMissing() {
    if (busy_ || !db_) return;
    if (!QFileInfo::exists(mdbPath())) {
        emit missingReady({}, "HRD Logbook's log file not found");
        return;
    }
    busy_ = true;
    // The console side is read here, on our own thread, before the worker
    // starts: LogDb's connection belongs to this thread.
    const QList<Qso> mine = db_->allQsos();
    QPointer<HrdSync> self(this);
    QThread* t = QThread::create([self, mine] {
        QString err;
        const auto have = readAllKeys(&err);
        QList<qint64> missing;
        if (err.isEmpty()) {
            for (const Qso& q : mine) {
                const qint64 secs = q.tsUtc.toUTC().toSecsSinceEpoch();
                bool found = false;
                for (const qint64 h :
                     have.value(q.call.trimmed().toUpper() + '|'
                                + q.band.trimmed().toUpper()))
                    if (std::llabs(h - secs) <= 600) { found = true; break; }
                if (!found) missing << q.id;
            }
        }
        if (!self) return;
        QMetaObject::invokeMethod(self.data(), [self, missing, err] {
            if (!self) return;
            self->busy_ = false;
            emit self->missingReady(missing,
                                    err.isEmpty() ? QString()
                                                  : "can't read HRD's log: " + err);
        }, Qt::QueuedConnection);
    });
    connect(t, &QThread::finished, t, &QObject::deleteLater);
    t->start(QThread::LowPriority);
}

QByteArray HrdSync::dbAddCommand(const Qso& q) {
    AdifRecord r = LogDb::toAdif(q);
    // HRD wants the frequency in Hz digits, no decimal point.
    if (r.contains("FREQ"))
        r["FREQ"] = QString::number(q.freqHz);
    QString cmd = "ver\rdb add {";
    for (auto it = r.constBegin(); it != r.constEnd(); ++it) {
        QString v = it.value();
        v.replace('"', '\'');            // a quote would end the value early
        cmd += it.key() + "=\"" + v + "\" ";
    }
    cmd += "}\rexit\r";
    return cmd.toUtf8();
}

bool HrdSync::hrdListening(int timeoutMs) {
    QTcpSocket s;
    s.connectToHost(hrdHost(), hrdPort());
    const bool ok = s.waitForConnected(timeoutMs);
    if (ok) {
        s.write("exit\r");
        s.flush();
        s.disconnectFromHost();
    }
    return ok;
}

void HrdSync::sendMissing(const QList<qint64>& ids) {
    if (!db_ || ids.isEmpty() || !sendQueue_.isEmpty()) return;
    if (!hrdListening()) {
        emit pushed(0, "HRD Logbook is not open — start it and try again");
        return;
    }
    sendQueue_ = ids;
    sent_ = 0;
    sendNext();
}

// One QSO per connection, paced, the way HRD's command port expects a
// client to talk: connect, ver, db add, exit.
void HrdSync::sendNext() {
    if (sendQueue_.isEmpty()) {
        emit pushed(sent_, QString());
        return;
    }
    const Qso q = db_->qso(sendQueue_.takeFirst());
    if (q.id < 0) { sendNext(); return; }
    const QByteArray cmd = dbAddCommand(q);
    auto* sock = new QTcpSocket(this);
    connect(sock, &QTcpSocket::connected, sock, [sock, cmd] {
        sock->write(cmd);
        sock->flush();
        sock->disconnectFromHost();
    });
    connect(sock, &QTcpSocket::disconnected, this, [this, sock] {
        sock->deleteLater();
        ++sent_;
        QTimer::singleShot(150, this, [this] { sendNext(); });
    });
    connect(sock, &QTcpSocket::errorOccurred, this,
            [this, sock](QAbstractSocket::SocketError e) {
        if (e == QAbstractSocket::RemoteHostClosedError) return;  // normal end
        const QString why = sock->errorString();
        sock->disconnect(this);          // no second report from disconnected
        sock->deleteLater();
        const int left = sendQueue_.size() + 1;
        sendQueue_.clear();
        emit pushed(sent_, QString("stopped with %1 left: %2").arg(left).arg(why));
    });
    sock->connectToHost(hrdHost(), hrdPort());
}

} // namespace ttc
