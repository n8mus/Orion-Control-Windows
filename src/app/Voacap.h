// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ui/PanadapterWidget.h"   // PropContour (the widget's data type)

#include <QObject>
#include <QPointF>
#include <QProcess>
#include <QString>
#include <QVector>

namespace ttc {

using PropContour = PanadapterWidget::PropContour;

// Driver for the real VOACAP engine (jawatson's voacapl port): writes a
// fixed-column card deck of point-to-point circuits on a radial grid
// around the QTH (24 azimuths x 17 distances), runs one voacapl process,
// parses the "S DBW" row of every circuit page and reduces the grid to
// KE9NS-style S-level contours. No engine -> engineMissing() explains,
// and nothing is ever faked. Inputs ride the console's live data: band
// center, NOAA sunspot number, the operator's TX power and grid.
class Voacap : public QObject {
    Q_OBJECT
public:
    explicit Voacap(QObject* parent = nullptr);

    // Empty when voacapl + ~/itshfbc are usable; else a human sentence.
    static QString engineMissing();

    bool busy() const { return proc_ && proc_->state() != QProcess::NotRunning; }

    // Fire a forecast for now (current UTC month/hour). powerW is the
    // transmitter's real drive. Silently ignored while a run is active.
    void request(double latDeg, double lonDeg, double freqMHz,
                 int ssn, int powerW);

signals:
    void ready(const QVector<PropContour>& contours, const QString& legend);

private:
    void onFinished(int code, QProcess::ExitStatus st);
    QProcess* proc_ = nullptr;
    double lat_ = 0.0, lon_ = 0.0, freq_ = 0.0;
    int ssn_ = 0, powerW_ = 0, hourUt_ = 0;
    QString runDir_;
};

} // namespace ttc
