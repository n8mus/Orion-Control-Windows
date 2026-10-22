// SPDX-License-Identifier: GPL-2.0-or-later
// cqrlog->console mirror test: the TSV-to-ADIF converter (column map,
// date/time forms, zone and confirmation-letter translation, NULL
// handling, watermark extraction) and the full round trip into LogDb —
// import once, near-dup skip on the re-pull. No cqrlog, no mysql.
#include <QBuffer>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <cstdio>

#include "log/Adif.h"
#include "log/CqrlogSync.h"
#include "log/LogDb.h"

using namespace ttc;

static int fails = 0;
#define CHECK(cond, what)                                                  \
    do {                                                                   \
        if (cond) {                                                        \
            std::printf("ok    %s\n", what);                               \
        } else {                                                           \
            std::printf("FAIL  %s\n", what);                               \
            ++fails;                                                       \
        }                                                                  \
    } while (0)

// One cqrlog row in mysql -N -B form, kColumns order:
// id qsodate time_on callsign freq mode band rst_s rst_r name qth loc
// waz itu qsl_r lotw_qslr eqsl_qsl_rcvd
static const char* kRow1 =
    "4711\t2026-09-07\t14:32\tOA4ENG\t7.074\tFT8\t40M\t-11\t-06\t"
    "Luis\tLima\tFH17MW\t10\t12\tQ\tL\tNULL\n";
static const char* kRow2 =
    "4712\t2026-09-07\t14:40\tZL1ABC\t7.076\tFT8\t40M\tNULL\tNULL\t"
    "NULL\tNULL\tNULL\t0\t0\tNULL\tNULL\tE\n";

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    qint64 maxId = 0;
    const QByteArray adif =
        CqrlogSync::rowsToAdif(QString(kRow1) + kRow2, &maxId);
    CHECK(maxId == 4712, "sync: watermark is the highest id");

    const QList<AdifRecord> recs = Adif::parseBytes(adif);
    CHECK(recs.size() == 2, "sync: two rows -> two ADIF records");
    if (recs.size() == 2) {
        const AdifRecord& r = recs[0];
        CHECK(r.value("CALL") == "OA4ENG", "sync: call maps");
        CHECK(r.value("QSO_DATE") == "20260907", "sync: date compacts");
        CHECK(r.value("TIME_ON") == "1432", "sync: time compacts");
        CHECK(r.value("BAND") == "40M" && r.value("MODE") == "FT8",
              "sync: band+mode map");
        CHECK(r.value("FREQ") == "7.074", "sync: freq stays MHz");
        CHECK(r.value("GRIDSQUARE") == "FH17MW", "sync: grid maps");
        CHECK(r.value("CQZ") == "10" && r.value("ITUZ") == "12",
              "sync: waz/itu become CQZ/ITUZ");
        CHECK(r.value("QSL_RCVD") == "Y" && r.value("LOTW_QSL_RCVD") == "Y"
                  && !r.contains("EQSL_QSL_RCVD"),
              "sync: Q/L letters translate, absent E stays absent");
        const AdifRecord& r2 = recs[1];
        CHECK(!r2.contains("NAME") && !r2.contains("CQZ")
                  && r2.value("EQSL_QSL_RCVD") == "Y",
              "sync: NULLs and zero zones drop, E translates");
    }

    // Short/garbage rows are skipped, watermark untouched.
    qint64 junkId = 0;
    CHECK(CqrlogSync::rowsToAdif("mysql: connect error\n", &junkId).isEmpty()
              && junkId == 0,
          "sync: junk output yields nothing");

    // Round trip into a real LogDb: import, then the same rows again —
    // the near-duplicate check keeps the mirror idempotent.
    QTemporaryDir tmp;
    LogDb db;
    if (!db.open(tmp.filePath("log.sqlite"))) {
        std::printf("FAIL  cannot open temp LogDb\n");
        return 1;
    }
    QBuffer b1;
    b1.setData(adif);
    b1.open(QIODevice::ReadOnly);
    CHECK(db.importAdif(b1, nullptr) == 2, "sync: first pull adds both");
    QBuffer b2;
    b2.setData(adif);
    b2.open(QIODevice::ReadOnly);
    CHECK(db.importAdif(b2, nullptr) == 0, "sync: re-pull adds nothing");

    if (fails == 0) std::printf("\nall ok\n");
    return fails ? 1 : 0;
}
