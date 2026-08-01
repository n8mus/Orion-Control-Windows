// SPDX-License-Identifier: GPL-2.0-or-later
//
// Smith-chart render harness. Loads a swr.json (the console's own stored
// sweep format), renders one band's runs through the real SmithChartWidget
// offscreen, and writes a PNG for human inspection. Also sanity-checks the
// sign inference on whatever data it loaded.
//
//   QT_QPA_PLATFORM=offscreen ./smithtest <swr.json> <band> <out.png>
//
// Band is the bare label ("40", "20"...). Defaults: the operator's live
// file, band 40, smith.png in the CWD.

#include "ui/SmithChartWidget.h"

#include <QApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

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

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "cannot open %s\n", qPrintable(path));
        return 1;
    }
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    QVector<ttc::PanadapterWidget::SwrRun> runs;
    for (const QJsonValue& rv : root.value(band).toArray()) {
        ttc::PanadapterWidget::SwrRun run;
        const QJsonObject ro = rv.toObject();
        run.ts = qint64(ro.value("ts").toDouble());
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
        runs.append(run);
    }
    if (runs.isEmpty()) {
        std::fprintf(stderr, "no runs for band %s in %s\n",
                     qPrintable(band), qPrintable(path));
        return 1;
    }

    // Report what the sign inference makes of the newest vector run.
    for (const auto& run : runs) {
        QVector<double> absX;
        for (const auto& pt : run.pts)
            if (pt.zValid) absX.append(std::fabs(pt.xOhm));
        if (absX.size() < 2) continue;
        const QVector<int> sg = SmithChartWidget::inferXSigns(absX);
        int flips = 0;
        for (int i = 1; i < sg.size(); ++i) if (sg[i] != sg[i-1]) ++flips;
        std::printf("band %s: %d vector points, %d sign crossing(s)\n",
                    qPrintable(band), int(absX.size()), flips);
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
