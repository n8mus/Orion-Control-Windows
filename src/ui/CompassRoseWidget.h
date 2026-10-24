// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QImage>
#include <QString>
#include <QWidget>

namespace ttc {

// The compass rose as a sidebar box (it used to be painted into the
// waterfall's corner, where the contest logger's window covered it —
// operator, 2026-10-18). An azimuthal-equidistant world disc centered on
// the QTH (Thetis rose look): up = north, edge = the antipode, so a line
// from the center IS the great-circle path at that bearing. Day/night
// tinted, sun bearing tick, the rotor's actual heading as a cyan needle,
// the target bearing as the red pointer. The disc fills the box width.
// Left-click = manual heading ("MAN"), right-click = clear.
class CompassRoseWidget : public QWidget {
    Q_OBJECT
public:
    explicit CompassRoseWidget(QWidget* parent = nullptr);

    // Great-circle bearing (deg from north) and distance (km).
    static void greatCircle(double lat1d, double lon1d, double lat2d, double lon2d,
                            double& bearingDeg, double& distKm);

    // Station location (from the grid square) — the disc's center.
    void setQth(double latDeg, double lonDeg);
    // Swing the pointer to a target (spot clicks, the log window's DX).
    void pointAt(double lat, double lon, const QString& label);
    // Same, by bearing (the contest deck's SP/LP: LP is the antipodal
    // heading, no place on the map to point at). distKm < 0 = unknown.
    void pointBearing(double bearingDeg, double distKm, const QString& label);
    // The antenna's actual heading; -1 = no rotor / unknown (needle hidden).
    void setRotorAz(double azDeg);

    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    // The pointer moved (spot, log window or manual click; -1 = cleared).
    void bearingChanged(double bearingDeg, const QString& label);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    int discR() const;                 // disc radius for the current width
    QPoint center() const;

    double  qthLat_ = 42.5, qthLon_ = -83.0;   // EN82 until the grid is set
    double  bearing_ = -1.0;                   // pointer heading (-1 = none)
    double  distKm_  = -1.0;
    double  rotorAz_ = -1.0;
    QString label_;
    QImage  cache_;                            // projected disc
    qint64  cacheMinute_ = -1;                 // grayline advances
    QString cacheKey_;                         // size+QTH
};

} // namespace ttc
