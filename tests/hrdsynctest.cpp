// SPDX-License-Identifier: GPL-2.0-or-later
// HRD Logbook <-> console sync. Needs a COPY of an HRD Access log in
// HRDTEST_MDB (never point it at the live file). Uses a scratch station
// log in a temp dir; no network, nothing is sent to HRD.
#include <QBuffer>
#include <QCoreApplication>
#include <QEventLoop>
#include <QSettings>
#include <QTemporaryDir>
#include <QDir>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTimeZone>
#include <QUuid>
#include <QTimer>

#include <cstdio>

#include "log/HrdSync.h"
#include "log/LogDb.h"

using namespace ttc;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("FAIL: "); \
    std::printf(__VA_ARGS__); std::printf("\n"); } else { \
    std::printf("ok:   "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static int importText(LogDb& db, const QByteArray& adif) {
    QBuffer b;
    b.setData(adif);
    b.open(QIODevice::ReadOnly);
    return db.importAdif(b, nullptr);
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName("n8mus-hrdsynctest");
    QCoreApplication::setApplicationName("hrdsynctest");
    QSettings().clear();

    QTemporaryDir tmp;
    LogDb db;
    CHECK(db.open(tmp.filePath("log.sqlite")), "scratch station log opens");

    // ---- phone: USB/LSB from QRZ or HRD lands on SSB ------------------
    importText(db, "<CALL:4>K1AB <QSO_DATE:8>20261101 <TIME_ON:4>1200 "
                   "<BAND:3>40M <MODE:3>SSB <EOR>");
    const int again = importText(db,
        "<CALL:4>K1AB <QSO_DATE:8>20261101 <TIME_ON:4>1201 "
        "<BAND:3>40m <MODE:3>LSB <EOR>"
        "<CALL:4>K2CD <QSO_DATE:8>20261101 <TIME_ON:4>1300 "
        "<BAND:3>20M <MODE:3>USB <EOR>");
    CHECK(again == 1, "LSB copy of an SSB contact is a duplicate (added %d of 2)", again);
    bool ssb = false;
    for (const Qso& q : db.allQsos())
        if (q.call == "K2CD") ssb = q.mode == "SSB";
    CHECK(ssb, "USB contact stored as SSB");
    {   // LoTW reports HRD-uploaded phone as USB: must confirm the SSB row.
        AdifRecord r;
        r.insert("CALL", "K2CD"); r.insert("BAND", "20m"); r.insert("MODE", "USB");
        r.insert("QSO_DATE", "20261101"); r.insert("QSL_RCVD", "Y");
        CHECK(db.applyLotwConfirmations({r}) == 1, "LoTW USB confirmation lands on the SSB contact");
    }
    QSettings().setValue("up/lotw/qslSince", "2026-11-10");
    importText(db, "<CALL:4>K3EF <QSO_DATE:8>20260601 <TIME_ON:4>1200 "
                   "<BAND:3>40M <MODE:2>CW <EOR>");
    CHECK(!QSettings().contains("up/lotw/qslSince"),
          "an import that adds contacts makes the next LoTW download fetch everything");

    // ---- db add command text -----------------------------------------
    Qso q;
    q.call = "W1AW";
    q.tsUtc = QDateTime(QDate(2026, 11, 11), QTime(18, 7, 15), QTimeZone::utc());
    q.band = "40M"; q.mode = "FT8"; q.freqHz = 7074920;
    q.name = "say \"hi\"";
    const QByteArray cmd = HrdSync::dbAddCommand(q);
    CHECK(cmd.startsWith("ver\rdb add {") && cmd.endsWith("}\rexit\r"),
          "db add framing");
    CHECK(cmd.contains("FREQ=\"7074920\""), "frequency in Hz digits");
    CHECK(cmd.contains("NAME=\"say 'hi'\""), "quotes in values can't end the value");

    // ---- the real thing, on a copy -----------------------------------
    const QString mdb = QString::fromLocal8Bit(qgetenv("HRDTEST_MDB"));
    if (mdb.isEmpty()) {
        std::printf("skip: HRDTEST_MDB not set — no HRD log copy to read\n");
    } else {
        QSettings().setValue("log/hrdMdb", mdb);
        CHECK(HrdSync::mdbPath() == mdb, "log/hrdMdb overrides the path");
        HrdSync sync(&db, nullptr);
        const int before = db.count();
        int pulled = -1;
        QEventLoop loop;
        QObject::connect(&sync, &HrdSync::pulled, &loop, [&](int n) { pulled = n; loop.quit(); });
        QTimer::singleShot(120000, &loop, &QEventLoop::quit);
        sync.pullSoon(0);
        loop.exec();
        const qint64 key = QSettings().value("log/hrdSyncKey").toLongLong();
        CHECK(pulled > 0, "first pull added %d contacts (log %d -> %d)", pulled, before, db.count());
        CHECK(key > 0, "watermark saved (%lld)", key);

        // Second pull from scratch must add nothing: everything is a dup.
        QSettings().setValue("log/hrdSyncKey", 0);
        HrdSync sync2(&db, nullptr);
        int pulled2 = 0;
        const int mid = db.count();
        QTimer::singleShot(0, &sync2, [&] { sync2.pullSoon(0); });
        QEventLoop loop2;
        QObject::connect(&sync2, &HrdSync::pulled, &loop2, [&](int n) { pulled2 = n; loop2.quit(); });
        QTimer::singleShot(60000, &loop2, &QEventLoop::quit);
        loop2.exec();
        CHECK(pulled2 == 0 && db.count() == mid, "re-pull from zero adds nothing (%d)", pulled2);

        // Compare: everything just pulled is in HRD, so the only contacts
        // HRD "lacks" are the three made up above.
        QList<qint64> missing; QString err = "timeout";
        QEventLoop loop3;
        QObject::connect(&sync2, &HrdSync::missingReady, &loop3,
                         [&](const QList<qint64>& ids, const QString& e) {
            missing = ids; err = e; loop3.quit();
        });
        QTimer::singleShot(120000, &loop3, &QEventLoop::quit);
        sync2.checkMissing();
        loop3.exec();
        CHECK(err.isEmpty(), "compare ran (%s)", qPrintable(err.isEmpty() ? "ok" : err));
        CHECK(missing.size() == 3, "HRD lacks only the 3 test contacts (got %d)", int(missing.size()));

        // ---- an edit corrects HRD's copy in place (on the COPY) ------
        // The command port is pointed at nothing, so neither the
        // not-found add nor the refresh can reach a real HRD.
        QSettings().setValue("up/hrd/port", 9);
        Qso orig;
        for (const Qso& x : db.allQsos())
            if (x.call == "Z335MK") orig = x;
        CHECK(orig.id >= 0, "found Z335MK (the newest HRD contact) in the pulled log");
        Qso now = orig;
        now.rstR = "579";
        now.comment = "edited by hrdsynctest";
        const auto hrdRows = [&](const QString& call) {
            QList<QStringList> rows;
            const QString c = "t-" + QUuid::createUuid().toString();
            {
                QSqlDatabase d = QSqlDatabase::addDatabase("QODBC", c);
                d.setDatabaseName("DRIVER={Microsoft Access Driver (*.mdb, *.accdb)};DBQ="
                                  + QDir::toNativeSeparators(mdb) + ";ReadOnly=1;");
                if (d.open()) {
                    QSqlQuery q(d);
                    q.exec("SELECT COL_RST_RCVD, COL_COMMENT, COL_NAME FROM"
                           " TABLE_HRD_CONTACTS_V01 WHERE COL_CALL='" + call + "'");
                    while (q.next())
                        rows << QStringList{q.value(0).toString(), q.value(1).toString(),
                                            q.value(2).toString()};
                }
            }
            QSqlDatabase::removeDatabase(c);
            return rows;
        };
        const auto rowsBefore = hrdRows("Z335MK");
        const QString msg = HrdSync::updateEdited(orig, now);
        const auto rowsAfter = hrdRows("Z335MK");
        CHECK(msg.contains("corrected in place"), "edit reported: %s", qPrintable(msg));
        CHECK(rowsAfter.size() == rowsBefore.size() && rowsAfter.size() == 1,
              "still exactly one Z335MK in HRD (%d -> %d)",
              int(rowsBefore.size()), int(rowsAfter.size()));
        CHECK(!rowsAfter.isEmpty() && rowsAfter[0][0] == "579"
                  && rowsAfter[0][1] == "edited by hrdsynctest",
              "HRD's row now carries the edit");
        CHECK(!rowsAfter.isEmpty() && rowsAfter[0][2] == rowsBefore.value(0).value(2),
              "fields the console left alone keep HRD's value (name)");
        {   // Time didn't move: the compare still finds Z335MK in HRD.
            QList<qint64> again; QString e2 = "timeout";
            QEventLoop l4;
            QObject::connect(&sync2, &HrdSync::missingReady, &l4,
                             [&](const QList<qint64>& ids, const QString& e) {
                again = ids; e2 = e; l4.quit();
            });
            QTimer::singleShot(120000, &l4, &QEventLoop::quit);
            sync2.checkMissing();
            l4.exec();
            CHECK(e2.isEmpty() && !again.contains(orig.id),
                  "edited row kept its UTC time (compare still matches it)");
        }
        Qso ghost = orig;
        ghost.call = "N0NE";
        const QString m2 = HrdSync::updateEdited(ghost, ghost);
        CHECK(m2.contains("HRD is closed"), "no match + no HRD: nothing written (%s)", qPrintable(m2));
    }

    QSettings().clear();
    std::printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "PASSED", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
