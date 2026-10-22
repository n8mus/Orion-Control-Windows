// SPDX-License-Identifier: GPL-3.0-or-later
// Not1MM dupe sheet against a synthetic Not1MM install in a temp dir:
//   cmake --build build --target not1mmtest && ./build/not1mmtest
// not1mm.json picks contest 7 in ham.db; DXLOG holds QSOs for contest 7
// and an old contest 3; the plugin file sets dupe_type. Checks the three
// rules, the other contest's QSOs never counting, the Band fallback, and
// that nothing is a dupe while Not1MM isn't running.
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <cstdio>

#include "log/Not1mmLog.h"

using namespace ttc;

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++fails;
}

static void writeFile(const QString& path, const QByteArray& data) {
    QFile f(path);
    f.open(QIODevice::WriteOnly | QIODevice::Truncate);
    f.write(data);
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc > 1 && QString(argv[1]) == "--real") {
        // This station's own Not1MM, read-only: what the console would see.
        Not1mmLog n;
        n.reload();
        std::printf("  contest %s, dupe_type %d, %d QSOs\n", qPrintable(n.contestName()),
                    n.dupeType(), n.qsoCount());
        return 0;
    }
    QTemporaryDir tmp;
    const QString cfg = tmp.path() + "/cfg", data = tmp.path() + "/data",
                  plug = tmp.path() + "/plugins";
    QDir().mkpath(cfg);
    QDir().mkpath(data);
    QDir().mkpath(plug);
    writeFile(cfg + "/not1mm.json", R"({"current_database":"ham.db","contest":7})");
    {
        QSqlDatabase d = QSqlDatabase::addDatabase("QSQLITE", "mk");
        d.setDatabaseName(data + "/ham.db");
        d.open();
        QSqlQuery q(d);
        q.exec("create table ContestInstance (ContestName text, ContestNR integer)");
        q.exec("insert into ContestInstance values ('cwo', 7), ('cq_ww_cw', 3)");
        q.exec("create table DXLOG (Call text, Freq real, Band real, Mode text, ContestNR integer)");
        q.exec("insert into DXLOG values ('W4LT', 14018.65, 14.0, 'CW', 7),"
               " ('ab5se', 7022.9, 7.0, 'CW', 7),"
               " ('K1ABC', 0, 21.0, 'USB', 7),"      // no Freq: Band fallback
               " ('DL1XX', 14025.0, 14.0, 'CW', 3)");   // another contest
        d.close();
    }
    QSqlDatabase::removeDatabase("mk");

    for (int type : {2, 1, 3}) {
        writeFile(plug + "/cwo.py", QByteArray("name = \"CWOPS\"\ndupe_type = ")
                                        + QByteArray::number(type) + "\n");
        Not1mmLog n(nullptr, cfg, data, plug);
        n.setEnabled(true);
        n.reload();
        n.setRunningForTest(false);
        if (type == 2) {
            check(n.contestName() == "cwo" && n.qsoCount() == 3, "reads contest 7's 3 QSOs only");
            check(!n.isDupe("W4LT", 14020000, "CW"), "not running: never a dupe");
        }
        n.setRunningForTest(true);
        std::printf(" dupe_type %d\n", type);
        check(n.dupeType() == type, "rule read from the plugin");
        check(!n.isDupe("DL1XX", 14025000, "CW"), "another contest's QSO is not a dupe");
        switch (type) {
        case 2:
            check(n.isDupe("W4LT", 14030000, "CW"), "same call, same band");
            check(!n.isDupe("W4LT", 7030000, "CW"), "same call, other band");
            check(n.isDupe("AB5SE", 7001000, "CW"), "case-blind call");
            check(n.isDupe("K1ABC", 21300000, "SSB"), "Band fallback when Freq is 0");
            break;
        case 1:
            check(n.isDupe("W4LT", 3530000, "CW"), "call only: any band");
            break;
        case 3:
            check(n.isDupe("K1ABC", 21250000, "SSB"), "band + mode group (USB = SSB = PH)");
            check(!n.isDupe("K1ABC", 21020000, "CW"), "same band, other mode");
            break;
        }
    }
    {
        writeFile(plug + "/cwo.py", "dupe_type = 4\n");
        Not1mmLog n(nullptr, cfg, data, plug);
        n.setEnabled(true);
        n.reload();
        n.setRunningForTest(true);
        check(!n.active() && !n.isDupe("W4LT", 14020000, "CW"), "dupe_type 4: no dupes");
    }
    if (fails == 0) std::printf("not1mmtest: all checks passed\n");
    return fails ? 1 : 0;
}
