// SPDX-License-Identifier: GPL-2.0-or-later
//
// Smith-chart render harness. Loads a swr.json (the console's own stored
// sweep format, v2: {"version":2,"runs":[{"ant","band","ts","notes","pts"}]})
// — an unversioned (pre-antenna) file also loads, treated as the
// unlabeled antenna, same migration the app itself does — renders one
// antenna+band's runs through the real SmithChartWidget offscreen, and
// writes a PNG for human inspection. Also sanity-checks the sign
// inference on whatever data it loaded.
//
//   QT_QPA_PLATFORM=offscreen ./smithtest <swr.json> <band> <out.png> [ant]
//
// Band is the bare label ("40", "20"...). ant defaults to "" (unlabeled).
// Defaults: the operator's live file, band 40, smith.png in the CWD.

#include "ui/SmithChartWidget.h"

#include <QApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>
#include <cstdio>

using ttc::SmithChartWidget;

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    const QString path = argc > 1 ? argv[1]
        : QStandardPaths::writableLocation(QStandardPaths::HomeLocation)
              + "/.local/share/n8mus/tentec-console/swr.json";
    const QString band = argc > 2 ? argv[2] : "40";
    const QString out  = argc > 3 ? argv[3] : "smith.png";
    const QString ant  = argc > 4 ? argv[4] : QString();

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "cannot open %s\n", qPrintable(path));
        return 1;
    }
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();

    auto runFromJson = [](const QJsonObject& ro) {
        ttc::PanadapterWidget::SwrRun run;
        run.ts    = qint64(ro.value("ts").toDouble());
        run.ant   = ro.value("ant").toString();
        run.band  = ro.value("band").toString();
        run.notes = ro.value("notes").toString();
        for (const QJsonValue& pv : ro.value("pts").toArray()) {
            const QJsonArray pa = pv.toArray();
            ttc::PanadapterWidget::SwrRun::Pt pt;
            pt.hz  = qint64(pa.at(0).toDouble());
            pt.swr = pa.at(1).toDouble();
            if (pa.size() >= 4) {
                pt.rOhm = pa.at(2).toDouble();
                pt.xOhm = pa.at(3).toDouble();
                pt.zValid = true;
            }
            run.pts.append(pt);
        }
        return run;
    };

    QVector<ttc::PanadapterWidget::SwrRun> runs;
    if (root.contains("version")) {
        for (const QJsonValue& rv : root.value("runs").toArray()) {
            ttc::PanadapterWidget::SwrRun run = runFromJson(rv.toObject());
            if (run.band == band && run.ant == ant) runs.append(run);
        }
    } else {
        // Pre-antenna file: every run is the unlabeled antenna.
        for (const QJsonValue& rv : root.value(band).toArray()) {
            ttc::PanadapterWidget::SwrRun run = runFromJson(rv.toObject());
            run.band = band;
            if (ant.isEmpty()) runs.append(run);
        }
    }
    if (runs.isEmpty()) {
        std::fprintf(stderr, "no runs for antenna \"%s\" band %s in %s\n",
                     qPrintable(ant), qPrintable(band), qPrintable(path));
        return 1;
    }
    std::sort(runs.begin(), runs.end(),
              [](const auto& a, const auto& b) { return a.ts > b.ts; });

    // Report what the sign inference makes of the newest vector run.
    for (const auto& run : runs) {
        QVector<double> absX, rr;
        for (const auto& pt : run.pts)
            if (pt.zValid) {
                absX.append(std::fabs(pt.xOhm));
                rr.append(pt.rOhm);
            }
        if (absX.size() < 2) continue;
        const QVector<int> sg = SmithChartWidget::inferXSigns(absX, rr);
        int flips = 0;
        for (int i = 1; i < sg.size(); ++i) if (sg[i] != sg[i-1]) ++flips;
        std::printf("band %s: %d vector points, %d sign crossing(s), "
                    "starts %s\n",
                    qPrintable(band), int(absX.size()), flips,
                    sg.first() < 0 ? "capacitive (-X)" : "inductive (+X)");
        break;
    }

    SmithChartWidget w;
    w.resize(600, 660);
    w.setRuns(runs, band);
    if (!w.grab().save(out)) {
        std::fprintf(stderr, "could not write %s\n", qPrintable(out));
        return 1;
    }
    std::printf("wrote %s\n", qPrintable(out));
    return 0;
}
