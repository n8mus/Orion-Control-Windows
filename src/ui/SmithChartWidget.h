// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ui/PanadapterWidget.h"   // SwrRun — the sweep's stored points

#include <QVector>
#include <QWidget>

namespace ttc {

// Smith chart of SWR-sweep runs measured with the LP-100A (vector meter
// runs carry R and X per point; radio-only runs can't plot here).
//
// The one subtlety is the SIGN of X. The LP-100A's coupler measures the
// magnitude of the impedance phase, not its direction — the operator's
// real 40 m sweep (2026-08-01) shows |X| dipping to ~4 ohms at resonance
// and rising on BOTH sides, never negative. TelePost's own Plot program
// has the same problem and solves it the same way we do: infer the sign
// from the sweep's shape ("based on impedance and phase slopes", per
// their help), and let the operator click any point that came out wrong
// to flip it. Below a series resonance an antenna is capacitive (X < 0),
// above it inductive, so each deep local minimum of |X| is a crossing
// and the sign alternates between crossings, starting capacitive.
//
// Everything is presentation: stored runs keep the meter's raw unsigned
// X, and the inference (plus any manual flips) lives only in this view.
class SmithChartWidget : public QWidget {
public:
    explicit SmithChartWidget(QWidget* parent = nullptr);

    void setRuns(const QVector<PanadapterWidget::SwrRun>& runs,
                 const QString& bandLabel);

    // Sign inference, exposed for the sweep-done resonance report: +1/-1
    // per point. |X| dips locate the crossings; the overall orientation is
    // chosen so the locus rotates clockwise with rising frequency (needs R
    // for that, hence both vectors). See the .cpp for why that is the
    // physically forced choice.
    static QVector<int> inferXSigns(const QVector<double>& absX,
                                    const QVector<double>& rOhm);

    QSize sizeHint() const override { return {580, 620}; }

protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    struct Pt { double fHz = 0, rOhm = 0, xAbs = 0, swr = 1; };
    QVector<Pt>  pts_;        // newest run, zValid points only
    QVector<int> signs_;      // ±1 per point (inferred, then user flips)
    QVector<Pt>  prevPts_;    // one older run, dim backdrop
    QVector<int> prevSigns_;
    QString      band_;
    qint64       ts_ = 0;     // newest run's timestamp
    int          hover_ = -1;

    // Paint-time geometry, kept for hit-testing the locus.
    QVector<QPointF> xy_;
    QPointF gamma(const Pt& p, int sign, QPointF c, double R) const;
    int nearestPoint(const QPointF& pos) const;
};

} // namespace ttc
