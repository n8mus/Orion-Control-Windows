// SPDX-License-Identifier: GPL-2.0-or-later
// TuneTable verification (tuner/amp settings store behind the pan's tuner
// box) — nearest-point lookup, same-kHz overwrite, clamping, JSON + file
// round trip. Pure logic:
//   cmake --build build --target tunetest && ./build/tunetest
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <cstdio>

#include "util/TuneTable.h"

using ttc::TunePoint;
using ttc::TuneTable;

static int fails = 0;

static void check(bool ok, const char* what) {
    if (!ok) {
        printf("FAIL: %s\n", what);
        ++fails;
    }
}

static TunePoint pt(qint64 hz, double cap, double ind = 10.0) {
    TunePoint p;
    p.hz = hz;
    p.cap = cap;
    p.ind = ind;
    p.ampTune = 5.0;
    p.ampLoad = 4.0;
    return p;
}

int main() {
    TuneTable t;
    check(t.nearest("dipole", "40", 7030000) == nullptr, "empty -> none");

    t.upsert("dipole", "40", pt(7025000, 6.5));
    t.upsert("dipole", "40", pt(7150000, 4.0));
    t.upsert("dipole", "40", pt(7075000, 5.5));

    const TunePoint* n = t.nearest("dipole", "40", 7030000);
    check(n && n->hz == 7025000, "nearest below");
    n = t.nearest("dipole", "40", 7140000);
    check(n && n->hz == 7150000, "nearest above");
    n = t.nearest("dipole", "40", 7000000);
    check(n && n->hz == 7025000, "below the lowest point");
    n = t.nearest("dipole", "40", 7290000);
    check(n && n->hz == 7150000, "above the highest point");
    n = t.nearest("dipole", "40", 7050000);        // 25 kHz from both
    check(n && n->hz == 7025000, "tie picks the lower point");

    // Buckets are separate per antenna AND per band.
    check(t.nearest("vertical", "40", 7030000) == nullptr, "other antenna empty");
    check(t.nearest("dipole", "20", 14030000) == nullptr, "other band empty");
    t.upsert("", "40", pt(7030000, 9.9));          // unlabeled antenna
    n = t.nearest("", "40", 7030000);
    check(n && n->cap == 9.9, "unlabeled antenna has its own bucket");
    n = t.nearest("dipole", "40", 7030000);
    check(n && n->cap == 6.5, "unlabeled did not leak into dipole");

    // Same kHz overwrites (newest data wins); a different kHz adds.
    t.upsert("dipole", "40", pt(7025400, 7.1));    // rounds to 7.025
    check(t.points("dipole", "40").size() == 3, "same kHz did not add");
    n = t.nearest("dipole", "40", 7025000);
    check(n && n->cap == 7.1, "same kHz replaced the values");
    t.upsert("dipole", "40", pt(7026000, 7.2));
    check(t.points("dipole", "40").size() == 4, "next kHz added an entry");
    const auto pts = t.points("dipole", "40");
    bool sorted = true;
    for (int i = 1; i < pts.size(); ++i) sorted &= pts[i - 1].hz < pts[i].hz;
    check(sorted, "points stay sorted");

    // Values clamp onto the real knobs.
    TunePoint wild = pt(7200000, 12.34, 44);
    wild.zPos = 9;
    wild.ampTune = 0.2;
    wild.ampLoad = 5.25;
    wild.swr = 0.5;                                 // no carrier = no SWR
    t.upsert("dipole", "40", wild);
    n = t.nearest("dipole", "40", 7200000);
    check(n && n->cap == 10.0, "cap clamps to 10.0");
    check(n && n->ind == 30.0, "inductor clamps to 30");
    check(n && n->zPos == 5, "Z position clamps to 5");
    check(n && n->ampTune == 1.0, "tune clamps to 1.0");
    check(n && n->ampLoad == 5.3, "load rounds to the tenth");
    check(n && n->swr == 0.0, "SWR below 1 means none");

    check(t.remove("dipole", "40", 7200300), "remove by kHz");
    check(!t.remove("dipole", "40", 7200000), "second remove finds nothing");

    // JSON + file round trip, including bypass, high Z, SWR and note.
    TunePoint b = pt(14250000, 3.3, 7.4);
    b.bypass = true;
    b.highZ = true;
    b.zPos = 4;
    b.swr = 1.234;
    b.note = "  best spot, phone end ";
    b.savedMs = 1760400000000;
    t.upsert("tribander", "20", b);

    QTemporaryDir dir;
    const QString path = dir.path() + "/sub/tuning.json";
    check(t.save(path), "save writes (and makes the folder)");
    TuneTable u;
    check(u.load(path), "load reads it back");
    check(u.size() == t.size(), "same point count after round trip");
    n = u.nearest("tribander", "20", 14250000);
    check(n && n->bypass && n->highZ && n->zPos == 4, "bypass + High Z 4 kept");
    check(n && n->swr == 1.23, "SWR kept to 2 places");
    check(n && n->ind == 7.4, "inductor tenths kept");
    check(n && n->note == "best spot, phone end", "note kept, trimmed");
    check(n && n->savedMs == 1760400000000, "timestamp kept");
    check(u.antennas() == QStringList({"", "dipole", "tribander"}),
          "antennas listed");

    TuneTable missing;
    check(missing.load(dir.path() + "/nope.json") && missing.isEmpty(),
          "missing file = empty table, not an error");
    QFile bad(dir.path() + "/bad.json");
    check(bad.open(QIODevice::WriteOnly), "write the corrupt fixture");
    bad.write("{ not json");
    bad.close();
    check(!missing.load(dir.path() + "/bad.json"), "corrupt file reports failure");

    if (fails == 0) printf("tunetest: all checks passed\n");
    return fails ? 1 : 0;
}
