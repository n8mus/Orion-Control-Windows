// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>

class QTcpSocket;

namespace ttc {

class CtyLookup;
class LogDb;
struct Qso;

// HRD Logbook <-> console sync: the Windows station's logger, in the seat
// cqrlog holds on the Linux box (operator, 2026-11-11). Same public shape
// as CqrlogSync so MainWindow wires either one the same way.
//
//  HRD -> console (automatic): HRD keeps its log in an Access file. We
//    READ it through the Microsoft Access ODBC driver, read-only, and
//    never write to it. Incremental on COL_PRIMARY_KEY (watermark in
//    QSettings log/hrdSyncKey); rows become ADIF and go through
//    LogDb::importAdif, whose near-duplicate check makes our own echoes
//    harmless and whose upload stamps keep HRD's history out of every
//    online-log push. The file can be read while HRD is closed.
//
//  console -> HRD: through HRD's own front door, its TCP command port
//    (7826, "db add {COL=...}"), so HRD does its own lookups and award
//    bookkeeping on what arrives. Live QSOs already go that way
//    (QslUploader::mirrorToLoggers); checkMissing/sendMissing are the
//    catch-up for whatever was logged while HRD was closed, and nothing is
//    sent without the operator's yes. That port only answers while HRD
//    Logbook is open.
class HrdSync : public QObject {
    Q_OBJECT
public:
    HrdSync(LogDb* db, const CtyLookup* cty, QObject* parent = nullptr);

    void start();                    // first pull soon + periodic re-pulls
    void pullSoon(int delayMs = 3000);

    void checkMissing();             // -> missingReady
    void sendMissing(const QList<qint64>& ids);   // -> pushed

    // HRD's Access file: log/hrdMdb, else HRD's default location.
    static QString mdbPath();
    // One QSO as an HRD command-port line ("db add {...}"). Shared with
    // QslUploader's live mirror.
    static QByteArray dbAddCommand(const Qso& q);
    // Is HRD Logbook's command port answering? (short blocking probe)
    static bool hrdListening(int timeoutMs = 800);

signals:
    void pulled(int added);
    void missingReady(const QList<qint64>& ids, const QString& error);
    void pushed(int sent, const QString& error);

private:
    void pull();
    void sendNext();

    LogDb* db_;
    const CtyLookup* cty_;
    QTimer timer_;
    bool busy_ = false;              // a read of HRD's file is in flight
    qint64 lastKey_ = 0;
    QList<qint64> sendQueue_;
    int sent_ = 0;
};

} // namespace ttc
