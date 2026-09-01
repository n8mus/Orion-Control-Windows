// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/GlobeWindow.h"

#include <QDateTime>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QtMath>
#include <cmath>

namespace ttc {

namespace {
// Earth from space: orthographic disc from the same classic map the
// compass rose uses, sun-lit (day/night from the real UTC sun) with limb
// shading for the sphere look.
QImage renderGlobe(int R, double lat0d, double lon0d) {
    static QImage earth;
    if (earth.isNull())
        earth = QImage(":/earth.jpg").convertToFormat(QImage::Format_RGB32);
    const int D = 2 * R;
    QImage img(D, D, QImage::Format_ARGB32);
    img.fill(Qt::transparent);
    if (earth.isNull()) return img;
    const double lat0 = qDegreesToRadians(lat0d);
    const double lon0 = qDegreesToRadians(lon0d);
    const QDateTime utc = QDateTime::currentDateTimeUtc();
    const double hours = utc.time().hour() + utc.time().minute() / 60.0;
    const double decl = 23.44
        * std::sin(2.0 * M_PI * (utc.date().dayOfYear() - 81) / 365.25)
        * M_PI / 180.0;
    const double subLon = (12.0 - hours) * 15.0 * M_PI / 180.0;
    for (int y = 0; y < D; ++y) {
        QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < D; ++x) {
            const double dx = (x + 0.5 - R) / R;
            const double dy = -(y + 0.5 - R) / R;      // screen y down
            const double rho = std::hypot(dx, dy);
            if (rho > 1.0) continue;
            const double c = std::asin(std::min(rho, 1.0));
            const double cosc = std::cos(c), sinc = std::sin(c);
            double la, lo;
            if (rho < 1e-9) {
                la = lat0;
                lo = lon0;
            } else {
                la = std::asin(cosc * std::sin(lat0)
                               + dy * sinc * std::cos(lat0) / rho);
                lo = lon0
                    + std::atan2(dx * sinc,
                                 rho * cosc * std::cos(lat0)
                                     - dy * sinc * std::sin(lat0));
            }
            int mx = static_cast<int>((lo / M_PI + 1.0) * 0.5
                                      * earth.width()) % earth.width();
            if (mx < 0) mx += earth.width();
            const int my = std::clamp(
                static_cast<int>((0.5 - la / M_PI) * earth.height()),
                0, earth.height() - 1);
            const QRgb col = earth.pixel(mx, my);
            const double sinElev = std::sin(la) * std::sin(decl)
                + std::cos(la) * std::cos(decl) * std::cos(lo - subLon);
            double b = sinElev > 0.0 ? 0.95 : 0.40;    // day / night
            b *= 0.55 + 0.45 * cosc;                   // sphere limb shading
            line[x] = qRgba(static_cast<int>(qRed(col) * b),
                            static_cast<int>(qGreen(col) * b),
                            static_cast<int>(qBlue(col) * b), 255);
        }
    }
    return img;
}

// Great-circle midpoint via vector slerp — where the camera should look.
void midpoint(double la1, double lo1, double la2, double lo2,
              double& laM, double& loM) {
    const auto rad = [](double d) { return qDegreesToRadians(d); };
    const double x1 = std::cos(rad(la1)) * std::cos(rad(lo1));
    const double y1 = std::cos(rad(la1)) * std::sin(rad(lo1));
    const double z1 = std::sin(rad(la1));
    const double x2 = std::cos(rad(la2)) * std::cos(rad(lo2));
    const double y2 = std::cos(rad(la2)) * std::sin(rad(lo2));
    const double z2 = std::sin(rad(la2));
    const double x = x1 + x2, y = y1 + y2, z = z1 + z2;
    const double n = std::sqrt(x * x + y * y + z * z);
    if (n < 1e-9) {                       // antipodes: camera on station
        laM = la1;
        loM = lo1;
        return;
    }
    laM = qRadiansToDegrees(std::asin(z / n));
    loM = qRadiansToDegrees(std::atan2(y / n, x / n));
}
} // namespace

GlobeWindow::GlobeWindow(QWidget* parent) : QDialog(parent) {
    setModal(false);
    setWindowTitle("Globe");
    resize(480, 500);
    setStyleSheet("QDialog { background: #0a0e15; }");
    setMouseTracking(false);
}

void GlobeWindow::setStations(double myLat, double myLon, double dxLat,
                              double dxLon, const QString& call) {
    myLat_ = myLat;
    myLon_ = myLon;
    const bool newDx = call != call_;
    dxLat_ = dxLat;
    dxLon_ = dxLon;
    call_ = call;
    haveDx_ = true;
    if (newDx) dragged_ = false;          // a fresh DX recenters the camera
    if (!dragged_)
        midpoint(myLat_, myLon_, dxLat_, dxLon_, viewLat_, viewLon_);
    update();
}

void GlobeWindow::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const int R = std::min(width(), height() - 24) / 2 - 10;
    const QPoint c(width() / 2, (height() - 24) / 2 + 0);
    const QString key = QString("%1|%2|%3|%4")
                            .arg(int(viewLat_ * 4)).arg(int(viewLon_ * 4))
                            .arg(R)
                            .arg(QDateTime::currentDateTimeUtc()
                                     .toString("yyyyMMddHH"));
    if (key != discKey_) {
        disc_ = renderGlobe(R, viewLat_, viewLon_);
        discKey_ = key;
    }
    p.drawImage(c.x() - R, c.y() - R, disc_);
    p.setPen(QPen(QColor(70, 90, 115), 1.5));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(c, R, R);

    const auto proj = [this, R, c](double la, double lo, QPointF& out) {
        const double la0 = qDegreesToRadians(viewLat_);
        const double lo0 = qDegreesToRadians(viewLon_);
        const double lar = qDegreesToRadians(la);
        const double lor = qDegreesToRadians(lo);
        const double cosc = std::sin(la0) * std::sin(lar)
            + std::cos(la0) * std::cos(lar) * std::cos(lor - lo0);
        if (cosc < 0.02) return false;
        out = QPointF(
            c.x() + R * std::cos(lar) * std::sin(lor - lo0),
            c.y() - R * (std::cos(la0) * std::sin(lar)
                         - std::sin(la0) * std::cos(lar)
                               * std::cos(lor - lo0)));
        return true;
    };

    if (haveDx_) {
        // Great-circle path, sampled; segments on the far side drop out.
        const auto rad = [](double d) { return qDegreesToRadians(d); };
        const double x1 = std::cos(rad(myLat_)) * std::cos(rad(myLon_));
        const double y1 = std::cos(rad(myLat_)) * std::sin(rad(myLon_));
        const double z1 = std::sin(rad(myLat_));
        const double x2 = std::cos(rad(dxLat_)) * std::cos(rad(dxLon_));
        const double y2 = std::cos(rad(dxLat_)) * std::sin(rad(dxLon_));
        const double z2 = std::sin(rad(dxLat_));
        const double dot = std::clamp(x1 * x2 + y1 * y2 + z1 * z2,
                                      -1.0, 1.0);
        const double ang = std::acos(dot);
        p.setPen(QPen(QColor(80, 200, 235, 230), 2.2));
        QPainterPath path;
        bool pen = false;
        const int N = 96;
        for (int i = 0; i <= N; ++i) {
            const double t = double(i) / N;
            const double sa = ang < 1e-9 ? 1.0 - t
                                         : std::sin((1.0 - t) * ang)
                                               / std::sin(ang);
            const double sb = ang < 1e-9 ? t
                                         : std::sin(t * ang)
                                               / std::sin(ang);
            const double x = sa * x1 + sb * x2;
            const double y = sa * y1 + sb * y2;
            const double z = sa * z1 + sb * z2;
            const double la = qRadiansToDegrees(
                std::asin(z / std::sqrt(x * x + y * y + z * z)));
            const double lo = qRadiansToDegrees(std::atan2(y, x));
            QPointF pt;
            if (proj(la, lo, pt)) {
                if (pen) path.lineTo(pt);
                else path.moveTo(pt);
                pen = true;
            } else {
                pen = false;
            }
        }
        p.drawPath(path);

        QPointF me, dx;
        if (proj(myLat_, myLon_, me)) {
            p.setPen(QPen(QColor(20, 60, 25), 1));
            p.setBrush(QColor(130, 222, 140));
            p.drawEllipse(me, 5, 5);
        }
        if (proj(dxLat_, dxLon_, dx)) {
            p.setPen(QPen(QColor(120, 70, 0), 1));
            p.setBrush(QColor(255, 190, 40));
            p.drawEllipse(dx, 5, 5);
            p.setPen(QColor(255, 220, 120));
            QFont f = p.font();
            f.setBold(true);
            p.setFont(f);
            p.drawText(dx + QPointF(9, 4), call_);
        }
    }

    // Footer.
    p.setPen(QColor(110, 125, 142));
    QFont f = p.font();
    f.setBold(false);
    f.setPointSize(8);
    p.setFont(f);
    p.drawText(QRect(0, height() - 22, width(), 20), Qt::AlignCenter,
               haveDx_ ? QString("%1 — drag to spin, double-click to "
                                 "recenter").arg(call_)
                       : "waiting for a call — drag to spin");
}

void GlobeWindow::mousePressEvent(QMouseEvent* e) {
    dragAt_ = e->pos();
}

void GlobeWindow::mouseMoveEvent(QMouseEvent* e) {
    if (!(e->buttons() & Qt::LeftButton)) return;
    const QPoint d = e->pos() - dragAt_;
    dragAt_ = e->pos();
    dragged_ = true;
    const int R = std::min(width(), height() - 24) / 2 - 10;
    viewLon_ += -d.x() * 90.0 / R;
    viewLat_ = std::clamp(viewLat_ + d.y() * 90.0 / R, -89.0, 89.0);
    while (viewLon_ > 180.0) viewLon_ -= 360.0;
    while (viewLon_ < -180.0) viewLon_ += 360.0;
    update();
}

void GlobeWindow::mouseDoubleClickEvent(QMouseEvent*) {
    dragged_ = false;
    if (haveDx_)
        midpoint(myLat_, myLon_, dxLat_, dxLon_, viewLat_, viewLon_);
    update();
}

} // namespace ttc
