// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/CompassRoseWidget.h"

#include <QDateTime>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <algorithm>
#include <cmath>

namespace ttc {

namespace {
constexpr int kMargin = 8;        // around the disc
constexpr int kReadoutH = 40;     // two text lines under it

// Azimuthal-equidistant world disc centered on the QTH: up = north, edge =
// the antipode. Day/night tinted from the current sun.
QImage renderRoseDisc(int R, double lat0d, double lon0d) {
    static QImage earth;                            // classic map, decoded once
    if (earth.isNull())
        earth = QImage(":/earth.jpg").convertToFormat(QImage::Format_RGB32);
    const int D = 2 * R;
    QImage img(D, D, QImage::Format_ARGB32);
    img.fill(Qt::transparent);
    if (earth.isNull()) return img;
    const double lat0 = lat0d * M_PI / 180.0, lon0 = lon0d * M_PI / 180.0;
    const QDateTime utc = QDateTime::currentDateTimeUtc();
    const double hours = utc.time().hour() + utc.time().minute() / 60.0;
    const double decl = 23.44 * std::sin(2.0 * M_PI * (utc.date().dayOfYear() - 81)
                                         / 365.25) * M_PI / 180.0;
    const double subLon = (12.0 - hours) * 15.0 * M_PI / 180.0;
    for (int y = 0; y < D; ++y) {
        QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < D; ++x) {
            const double dx = x + 0.5 - R, dy = y + 0.5 - R;
            const double r = std::hypot(dx, dy);
            if (r > R - 1.0) continue;
            const double c  = r / R * M_PI;
            const double th = std::atan2(dx, -dy);  // bearing from north
            const double la = std::asin(std::sin(lat0) * std::cos(c)
                              + std::cos(lat0) * std::sin(c) * std::cos(th));
            const double lo = lon0
                + std::atan2(std::sin(th) * std::sin(c) * std::cos(lat0),
                             std::cos(c) - std::sin(lat0) * std::sin(la));
            int mx = static_cast<int>((lo / M_PI + 1.0) * 0.5 * earth.width())
                     % earth.width();
            if (mx < 0) mx += earth.width();
            const int my = std::clamp(
                static_cast<int>((0.5 - la / M_PI) * earth.height()),
                0, earth.height() - 1);
            const QRgb col = earth.pixel(mx, my);
            const double sinElev = std::sin(la) * std::sin(decl)
                + std::cos(la) * std::cos(decl) * std::cos(lo - subLon);
            const double b = sinElev > 0.0 ? 0.95 : 0.42;   // day / night tint
            line[x] = qRgba(static_cast<int>(qRed(col) * b),
                            static_cast<int>(qGreen(col) * b),
                            static_cast<int>(qBlue(col) * b), 255);
        }
    }
    return img;
}
}  // namespace

void CompassRoseWidget::greatCircle(double lat1d, double lon1d, double lat2d,
                                    double lon2d, double& bearingDeg, double& distKm) {
    const double la1 = lat1d * M_PI / 180.0, la2 = lat2d * M_PI / 180.0;
    const double dLon = (lon2d - lon1d) * M_PI / 180.0;
    bearingDeg = std::atan2(std::sin(dLon) * std::cos(la2),
                            std::cos(la1) * std::sin(la2)
                                - std::sin(la1) * std::cos(la2) * std::cos(dLon))
                 * 180.0 / M_PI;
    if (bearingDeg < 0.0) bearingDeg += 360.0;
    const double c = std::acos(std::clamp(
        std::sin(la1) * std::sin(la2)
            + std::cos(la1) * std::cos(la2) * std::cos(dLon), -1.0, 1.0));
    distKm = 6371.0 * c;
}

CompassRoseWidget::CompassRoseWidget(QWidget* parent) : QWidget(parent) {
    setCursor(Qt::CrossCursor);
    setToolTip("Click: point the rose (the rotor turns there)\n"
               "Right-click: clear the pointer");
    QSizePolicy sp(QSizePolicy::Preferred, QSizePolicy::Fixed);
    sp.setHeightForWidth(true);
    setSizePolicy(sp);
    // The grayline creeps: re-project once a minute.
    auto* t = new QTimer(this);
    connect(t, &QTimer::timeout, this, [this] { if (isVisible()) update(); });
    t->start(60 * 1000);
}

int CompassRoseWidget::heightForWidth(int w) const {
    return std::max(80, w - 2 * kMargin) + kMargin + kReadoutH;
}
QSize CompassRoseWidget::sizeHint() const { return {280, heightForWidth(280)}; }
QSize CompassRoseWidget::minimumSizeHint() const { return {120, heightForWidth(120)}; }

void CompassRoseWidget::resizeEvent(QResizeEvent* e) {
    // A scroll-area column ignores height-for-width: pin the height to the
    // width so the disc stays round and nothing below it is squashed.
    const int h = heightForWidth(width());
    if (minimumHeight() != h) setFixedHeight(h);
    QWidget::resizeEvent(e);
}

int CompassRoseWidget::discR() const {
    return std::max(40, (width() - 2 * kMargin) / 2);
}
QPoint CompassRoseWidget::center() const {
    return {width() / 2, kMargin + discR()};
}

void CompassRoseWidget::setQth(double latDeg, double lonDeg) {
    qthLat_ = latDeg;
    qthLon_ = lonDeg;
    cacheKey_.clear();                             // disc re-projects
    update();
}

void CompassRoseWidget::pointAt(double lat, double lon, const QString& label) {
    greatCircle(qthLat_, qthLon_, lat, lon, bearing_, distKm_);
    label_ = label;
    emit bearingChanged(bearing_, label_);
    update();
}

void CompassRoseWidget::pointBearing(double bearingDeg, double distKm,
                                     const QString& label) {
    bearing_ = bearingDeg;
    distKm_ = distKm;
    label_ = label;
    emit bearingChanged(bearing_, label_);
    update();
}

void CompassRoseWidget::setRotorAz(double azDeg) {
    if (azDeg == rotorAz_) return;
    rotorAz_ = azDeg;
    update();
}

void CompassRoseWidget::mousePressEvent(QMouseEvent* e) {
    const QPoint c = center();
    const double dx = e->position().x() - c.x(), dy = c.y() - e->position().y();
    if (e->button() == Qt::RightButton) {
        bearing_ = -1.0;
        distKm_ = -1.0;
        label_.clear();
        emit bearingChanged(-1.0, QString());
        update();
        return;
    }
    if (e->button() != Qt::LeftButton) return;
    if (std::hypot(dx, dy) > discR()) return;      // the readout isn't a dial
    bearing_ = std::atan2(dx, dy) * 180.0 / M_PI;
    if (bearing_ < 0.0) bearing_ += 360.0;
    distKm_ = -1.0;
    label_ = "MAN";
    emit bearingChanged(bearing_, label_);
    update();
}

void CompassRoseWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(0x12, 0x19, 0x21));
    const int R = discR();
    const QPoint c = center();
    const int cx = c.x(), cy = c.y();
    const qint64 minute = QDateTime::currentSecsSinceEpoch() / 60;
    const QString key = QString("%1|%2|%3").arg(R).arg(qthLat_).arg(qthLon_);
    if (cacheKey_ != key || cacheMinute_ != minute) {
        cache_ = renderRoseDisc(R, qthLat_, qthLon_);
        cacheKey_ = key;
        cacheMinute_ = minute;
    }
    p.setRenderHint(QPainter::Antialiasing);
    p.drawImage(cx - R, cy - R, cache_);
    // Ring + ticks every 30 degrees, cardinal labels.
    p.setPen(QPen(QColor(70, 84, 100), 2));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(c, R, R);
    QFont f = font();
    f.setPixelSize(std::clamp(R / 9, 9, 14));
    f.setBold(true);
    p.setFont(f);
    for (int a = 0; a < 360; a += 30) {
        const double rad = a * M_PI / 180.0;
        const double sx = std::sin(rad), sy = -std::cos(rad);
        const bool cardinal = a % 90 == 0;
        p.setPen(QPen(QColor(150, 165, 182), cardinal ? 2 : 1));
        p.drawLine(QPointF(cx + sx * (R - (cardinal ? 10 : 5)),
                           cy + sy * (R - (cardinal ? 10 : 5))),
                   QPointF(cx + sx * R, cy + sy * R));
    }
    p.setPen(QColor(200, 212, 224));
    static const char* card[] = {"N", "E", "S", "W"};
    const double cr = R - f.pixelSize() - 8;
    for (int i = 0; i < 4; ++i) {
        const double rad = i * 90.0 * M_PI / 180.0;
        const QPointF pt(cx + std::sin(rad) * cr, cy - std::cos(rad) * cr);
        p.drawText(QRectF(pt.x() - 10, pt.y() - 10, 20, 20), Qt::AlignCenter, card[i]);
    }
    // Sun bearing tick (grayline aid): where to point for the terminator.
    {
        const QDateTime utc = QDateTime::currentDateTimeUtc();
        const double hours = utc.time().hour() + utc.time().minute() / 60.0;
        const double decl = 23.44
            * std::sin(2.0 * M_PI * (utc.date().dayOfYear() - 81) / 365.25);
        double sunLon = (12.0 - hours) * 15.0;
        while (sunLon > 180.0) sunLon -= 360.0;
        while (sunLon < -180.0) sunLon += 360.0;
        double sb, sd;
        greatCircle(qthLat_, qthLon_, decl, sunLon, sb, sd);
        const double rad = sb * M_PI / 180.0;
        p.setBrush(QColor(255, 190, 40));
        p.setPen(QPen(QColor(120, 70, 0), 1));
        p.drawEllipse(QPointF(cx + std::sin(rad) * (R - 5),
                              cy - std::cos(rad) * (R - 5)), 4, 4);
    }
    // Rotor needle (actual antenna heading): cyan, under the red target
    // pointer so a converged rotor reads as one needle with a cyan halo.
    if (rotorAz_ >= 0.0) {
        const double rad = rotorAz_ * M_PI / 180.0;
        const QPointF tip(cx + std::sin(rad) * (R - 6), cy - std::cos(rad) * (R - 6));
        p.setPen(QPen(QColor(80, 200, 235), 4, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(cx, cy), tip);
    }
    if (bearing_ >= 0.0) {
        const double rad = bearing_ * M_PI / 180.0;
        const QPointF tip(cx + std::sin(rad) * (R - 12), cy - std::cos(rad) * (R - 12));
        p.setPen(QPen(QColor(235, 80, 60), 3, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(cx, cy), tip);
    }
    // Home: the disc center IS the QTH.
    p.setPen(Qt::NoPen);
    p.setBrush(bearing_ >= 0.0 ? QColor(235, 80, 60) : QColor(255, 160, 160));
    p.drawEllipse(QPointF(cx, cy), 3.5, 3.5);

    // Readout under the disc: rotor heading left, the pointer's target right.
    QFont rf = font();
    rf.setPixelSize(12);
    rf.setBold(true);
    p.setFont(rf);
    const QFontMetrics fm(rf);
    const int y1 = cy + R + 6 + fm.ascent();
    const int y2 = y1 + fm.height() + 1;
    const int x0 = kMargin, x1 = width() - kMargin;
    p.setPen(QColor(143, 163, 184));
    p.drawText(x0, y1, "ROTOR");
    p.setPen(rotorAz_ >= 0.0 ? QColor(80, 200, 235) : QColor(90, 105, 120));
    p.drawText(x0 + fm.horizontalAdvance("ROTOR  "), y1,
               rotorAz_ >= 0.0 ? QString("%1°").arg(qRound(rotorAz_)) : QString("--"));
    if (bearing_ >= 0.0) {
        const QString l1 = label_.isEmpty() ? QString("MAN") : label_;
        const QString l2 = distKm_ >= 0.0
            ? QString("%1°  %2 km").arg(qRound(bearing_)).arg(qRound(distKm_))
            : QString("%1°").arg(qRound(bearing_));
        p.setPen(QColor(235, 120, 90));
        p.drawText(x1 - fm.horizontalAdvance(l1), y1, l1);
        p.setPen(QColor(220, 230, 240));
        p.drawText(x1 - fm.horizontalAdvance(l2), y2, l2);
    } else {
        const QString hint = "click the disc to aim";
        p.setPen(QColor(90, 105, 120));
        p.drawText(x1 - fm.horizontalAdvance(hint), y1, hint);
    }
}

} // namespace ttc
