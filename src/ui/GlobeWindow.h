// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QDialog>
#include <QImage>
#include <QPoint>
#include <QString>

namespace ttc {

// The 3D globe view from cqrlog's New QSO window, native: Earth rendered
// from space (orthographic, day/night from the real sun), the station and
// the DX both marked, the great-circle path arcing between them. The view
// opens centered on the path midpoint so both ends show; drag to spin.
class GlobeWindow : public QDialog {
    Q_OBJECT
public:
    explicit GlobeWindow(QWidget* parent = nullptr);

    // New pair to show: my QTH and the DX. Recenters on the midpoint
    // (a drag keeps manual control until the next call arrives).
    void setStations(double myLat, double myLon, double dxLat, double dxLon,
                     const QString& call);

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;

private:
    double myLat_ = 0, myLon_ = 0, dxLat_ = 0, dxLon_ = 0;
    bool haveDx_ = false;
    QString call_;
    double viewLat_ = 30.0, viewLon_ = -60.0;   // camera center
    bool dragged_ = false;                       // manual spin holds
    QPoint dragAt_;
    mutable QImage disc_;                        // cached render
    mutable QString discKey_;
};

} // namespace ttc
