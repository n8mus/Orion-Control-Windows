// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/PanadapterWidget.h"

#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QDateTime>
#include <QDesktopServices>
#include <QMouseEvent>
#include <QSettings>
#include <QUrl>
#include <QWheelEvent>
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>

namespace ttc {

namespace {
constexpr int   kWfHistRows   = 1024;    // raw dB rows kept for re-render on zoom
constexpr int   kMinViewSpanHz = 4000;   // deep enough to edit a CW filter
constexpr int   kScaleBandH    = 16;     // freq-scale strip between spectrum and wf
constexpr int   kDbAxisW       = 40;     // grab width of the left dB scale
constexpr float kPeakDecayDb   = 0.15f;  // peak-hold droop per frame (~2-3 dB/s)
constexpr float kFillGamma     = 0.6f;   // compresses the ramp: warm colors arrive
                                         // ~60% up the scale (KE9NS look)
constexpr float kNoRow         = -300.0f;

// Color palettes. All stops interpolate linearly; t=0 is the scale bottom
// (noise floor), t=1 the top. "Enhanced" is the vivid KE9NS-PowerSDR-style
// ramp — signals climb through blue/cyan/green into yellow/orange/red so
// strength reads as color, not just height.
struct Stop { float t; int r, g, b; };
const Stop kEnhanced[] = {
    {0.00f,   0,   0,   0}, {0.12f,  25,   0,  60}, {0.25f,   0,  30, 160},
    {0.40f,   0, 120, 230}, {0.52f,   0, 200, 180}, {0.62f,  40, 210,  50},
    {0.72f, 230, 230,  40}, {0.82f, 255, 140,  20}, {0.92f, 255,  40,  30},
    {1.00f, 255, 255, 255},
};
const Stop kClassic[] = {
    {0.00f,   0,   0,   0}, {0.20f,   0,   0, 120}, {0.40f,   0, 160, 200},
    {0.60f,   0, 200,  80}, {0.75f, 230, 220,  40}, {0.90f, 240,  60,  30},
    {1.00f, 255, 255, 255},
};
const Stop kThermal[] = {
    {0.00f,   0,   0,   0}, {0.30f,  90,   0,  10}, {0.55f, 200,  40,   0},
    {0.75f, 255, 150,   0}, {0.90f, 255, 230,  60}, {1.00f, 255, 255, 255},
};
const Stop kGray[] = {
    {0.00f,   0,   0,   0}, {1.00f, 255, 255, 255},
};
struct Palette { const Stop* stops; size_t n; };
const Palette kPalettes[] = {
    {kEnhanced, std::size(kEnhanced)},
    {kClassic,  std::size(kClassic)},
    {kThermal,  std::size(kThermal)},
    {kGray,     std::size(kGray)},
};

// A "nice" grid step (1/2/5 x 10^k) giving ~8 divisions across the view.
double niceStep(double span) {
    const double raw = span / 8.0;
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    for (double m : {1.0, 2.0, 5.0, 10.0})
        if (raw <= m * mag) return m * mag;
    return 10.0 * mag;
}
} // namespace

QStringList PanadapterWidget::paletteNames() {
    return {"Enhanced", "Classic", "Thermal", "Grayscale"};
}

QStringList PanadapterWidget::backgroundNames() {
    // Stored-enum rule: indices persist in settings — append only. 2..5 =
    // world-map backdrops (5 = "Map: Custom…", a FIXED index, image path in
    // display/mapCustomPath). 6 = "Ship" — artwork chosen BY NAME from the
    // DisplayPanel's fleet gallery (~/Pictures/starships; the filename is
    // the name), cover-cropped and dimmed under the trace (path lands in
    // display/shipImagePath; the Day slider doubles as brightness).
    // Code-drawn ship blueprints sailed here briefly and were struck from
    // the register as tacky.
    return {"Dark", "Blue Rays", "Map: Classic", "Map: Vegetation",
            "Map: Night lights", "Map: Custom…", "Ship"};
}

namespace {
// "Blue with lighting from above" (KE9NS): deep blue vertical gradient plus a
// few soft light shafts slanting down from the top, rendered once per size.
QImage renderBlueRays(int w, int h) {
    QImage img(w, h, QImage::Format_RGB32);
    QPainter p(&img);
    QLinearGradient base(0, 0, 0, h);
    base.setColorAt(0.0, QColor(16, 42, 78));
    base.setColorAt(0.55, QColor(8, 20, 40));
    base.setColorAt(1.0, QColor(3, 8, 16));
    p.fillRect(QRect(0, 0, w, h), base);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setCompositionMode(QPainter::CompositionMode_Plus);   // additive glow
    // Three rounded pools of light from above (KE9NS look): radial gradients
    // centered just above the top edge, so each lobe reads as a soft dome
    // rather than a hard-edged beam. Fixed table = never flickers on rebuild.
    struct Lamp { float x, r, bright; };
    static const Lamp lamps[] = {
        {0.18f, 0.70f, 1.00f}, {0.50f, 0.85f, 0.85f}, {0.82f, 0.65f, 0.95f},
    };
    for (const Lamp& L : lamps) {
        QRadialGradient g(QPointF(L.x * w, -0.10 * h), L.r * h);
        g.setColorAt(0.00, QColor(130, 190, 255, int(150 * L.bright)));
        g.setColorAt(0.40, QColor(90, 150, 230, int(60 * L.bright)));
        g.setColorAt(1.00, QColor(0, 0, 0, 0));
        p.fillRect(QRect(0, 0, w, h), g);
    }
    return img;
}

// "Ship: Custom…" backdrop: the operator's chosen artwork (any image on
// disk), cover-cropped to fill the spectrum area, dimmed to backdrop
// strength (brightPct = the Day slider), with the same fade-to-dark floor
// the maps use so the trace baseline stays readable. Nothing is committed
// to the repo — the path lives in QSettings display/shipImagePath.
QImage renderShipImage(int w, int h, const QImage& art, int brightPct) {
    if (art.isNull()) return renderBlueRays(w, h);
    QImage img(w, h, QImage::Format_RGB32);
    img.fill(QColor(6, 9, 16));
    QPainter p(&img);
    QImage scaled = art.scaled(w, h, Qt::KeepAspectRatioByExpanding,
                               Qt::SmoothTransformation);
    p.drawImage(QPoint((w - scaled.width()) / 2, (h - scaled.height()) / 2),
                scaled);
    // Dim to backdrop strength (Day slider %, same feel as the maps).
    const int dim = 255 - std::clamp(brightPct, 5, 100) * 255 / 100;
    p.fillRect(QRect(0, 0, w, h), QColor(4, 7, 14, dim));
    const int fadeH = std::min(h / 4, 60);
    QLinearGradient fade(0, h - fadeH, 0, h);
    fade.setColorAt(0.0, QColor(12, 16, 22, 0));
    fade.setColorAt(1.0, QColor(12, 16, 22, 235));
    p.fillRect(QRect(0, h - fadeH, w, fadeH), fade);
    return img;
}

// World map with live grayline: an equirectangular NASA basemap, shaded
// for use as a backdrop, with the night side darkened from the current UTC
// subsolar point (soft twilight band = the grayline). KE9NS proportions:
// the map fills only the upper band of the spectrum area and fades to dark
// where the trace floor lives. dayPct/nightPct = user brightness (percent).
QImage renderWorldMap(int w, int h, const QImage& earth,
                      int dayPct, int nightPct, bool drawSun,
                      int sfi, int aIdx, double kIdx) {
    if (earth.isNull()) return renderBlueRays(w, h);  // basemap missing
    const int mapH = std::max(1, static_cast<int>(h * 0.84));
    QImage img = earth.scaled(w, mapH, Qt::IgnoreAspectRatio,
                              Qt::SmoothTransformation);
    // Subsolar point from UTC (declination approx; equation of time ignored —
    // a display grayline, not an almanac).
    const QDateTime utc = QDateTime::currentDateTimeUtc();
    const int doy = utc.date().dayOfYear();
    const double hours = utc.time().hour() + utc.time().minute() / 60.0;
    const double decl = 23.44 * std::sin(2.0 * M_PI * (doy - 81) / 365.25)
                        * M_PI / 180.0;
    const double subLon = (12.0 - hours) * 15.0 * M_PI / 180.0;
    std::vector<double> sinLatSinD(mapH), cosLatCosD(mapH), cosDLon(w);
    for (int y = 0; y < mapH; ++y) {
        const double lat = (90.0 - 180.0 * (y + 0.5) / mapH) * M_PI / 180.0;
        sinLatSinD[y] = std::sin(lat) * std::sin(decl);
        cosLatCosD[y] = std::cos(lat) * std::cos(decl);
    }
    for (int x = 0; x < w; ++x) {
        const double lon = (360.0 * (x + 0.5) / w - 180.0) * M_PI / 180.0;
        cosDLon[x] = std::cos(lon - subLon);
    }
    // KE9NS grayline zones (spot.cs SUNANGLE/Darken): flat brightness steps,
    // not a glow — day above the horizon, a wide dusk/dawn band while the sun
    // sits 0..6 degrees below (SZA 90..96), dark night core beyond that.
    // Boundary smoothsteps widened (Thetis-style soft terminator) so the
    // zones blend instead of showing hard edges. Levels are user-set: night
    // floor at nightPct, full day at dayPct, dusk band 45 % of the way up.
    const double duskLo = std::sin(-6.0 * M_PI / 180.0);   // night below this
    const double nLvl = std::clamp(nightPct, 0, 100) / 100.0;
    const double dLvl = std::clamp(dayPct, 10, 120) / 100.0;
    auto smooth = [](double lo, double hi, double v) {
        const double t = std::clamp((v - lo) / (hi - lo), 0.0, 1.0);
        return t * t * (3.0 - 2.0 * t);
    };
    for (int y = 0; y < mapH; ++y) {
        QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const double sinElev = sinLatSinD[y] + cosLatCosD[y] * cosDLon[x];
            const double tDusk = smooth(duskLo - 0.05, duskLo + 0.05, sinElev);
            const double tDay  = smooth(-0.04, 0.04, sinElev);
            const double bright = nLvl + (dLvl - nLvl) * (0.45 * tDusk + 0.55 * tDay);
            const int f = static_cast<int>(bright * 256.0);
            const QRgb c = line[x];
            line[x] = qRgb(std::min(255, (qRed(c)   * f) >> 8),
                           std::min(255, (qGreen(c) * f) >> 8),
                           std::min(255, (qBlue(c)  * f) >> 8));
        }
    }
    // Sun marker at the subsolar point (already computed for the grayline),
    // with the KE9NS/Thetis-style space-weather caption beside it. Rides the
    // same "Solar data panel" toggle as the corner readout.
    if (drawSun) {
        const double latDeg = decl * 180.0 / M_PI;
        double lonDeg = (12.0 - hours) * 15.0;
        while (lonDeg > 180.0) lonDeg -= 360.0;
        while (lonDeg < -180.0) lonDeg += 360.0;
        const double sx = (lonDeg + 180.0) / 360.0 * w;
        const double sy = (90.0 - latDeg) / 180.0 * mapH;
        QPainter sp(&img);
        sp.setRenderHint(QPainter::Antialiasing);
        QRadialGradient glow(QPointF(sx, sy), 24);
        glow.setColorAt(0.0, QColor(255, 170, 40, 160));
        glow.setColorAt(1.0, QColor(255, 170, 40, 0));
        sp.setPen(Qt::NoPen);
        sp.setBrush(glow);
        sp.drawEllipse(QPointF(sx, sy), 24, 24);
        sp.setBrush(QColor(255, 176, 32));
        sp.setPen(QPen(QColor(120, 70, 0), 1));
        sp.drawEllipse(QPointF(sx, sy), 5, 5);
        if (sfi > 0) {
            QFont f = sp.font();
            f.setPixelSize(10);
            f.setBold(true);
            sp.setFont(f);
            QString cap = QString("SFI %1").arg(sfi);
            if (aIdx >= 0) cap += QString("  A %1").arg(aIdx);
            if (kIdx >= 0) cap += QString("  K %1").arg(kIdx, 0, 'f', 1);
            const int tw = sp.fontMetrics().horizontalAdvance(cap);
            double tx = sx + 12;                    // flip side near the edge
            if (tx + tw > w - 4) tx = sx - 12 - tw;
            sp.setPen(QColor(0, 0, 0, 190));        // shadow for readability
            sp.drawText(QPointF(tx + 1, sy + 4 + 1), cap);
            sp.setPen(QColor(255, 214, 90));
            sp.drawText(QPointF(tx, sy + 4), cap);
        }
    }
    // Composite: map up top, dark trace floor below, soft fade between.
    QImage out(w, h, QImage::Format_RGB32);
    out.fill(QColor(12, 16, 22));
    QPainter p(&out);
    p.drawImage(0, 0, img);
    const int fadeH = std::min(mapH / 4, 60);
    QLinearGradient fade(0, mapH - fadeH, 0, mapH);
    fade.setColorAt(0.0, QColor(12, 16, 22, 0));
    fade.setColorAt(1.0, QColor(12, 16, 22, 255));
    p.fillRect(QRect(0, mapH - fadeH, w, fadeH), fade);
    return out;
}
} // namespace

const QImage& PanadapterWidget::backgroundImage(int w, int h) {
    const bool isMap = ds_.background >= 2 && ds_.background <= 5;
    const bool isShip = ds_.background == 6;       // Ship: Custom…
    const qint64 minute = isMap
        ? QDateTime::currentSecsSinceEpoch() / 60 : 0;      // grayline advances
    if (bgMode_ != ds_.background || bgW_ != w || bgH_ != h
        || bgMinute_ != minute
        || bgDay_ != ds_.mapDay || bgNight_ != ds_.mapNight
        || bgSun_ != ds_.showSolar
        || (isShip && QSettings().value("display/shipImagePath").toString()
                          != mapSrcKey_)) {      // re-pick applies live
        if (isMap) {
            // Pick + decode the basemap only when the source changes; the
            // custom entry re-reads its path so a new pick applies live.
            QString key;
            switch (ds_.background) {
                case 2: key = ":/earth.jpg"; break;
                case 3: key = ":/earth_veg.jpg"; break;
                case 4: key = ":/earth_night.jpg"; break;
                default:
                    key = QSettings().value("display/mapCustomPath").toString();
            }
            if (key != mapSrcKey_) {
                mapSrc_ = QImage(key).convertToFormat(QImage::Format_RGB32);
                mapSrcKey_ = key;
            }
            bgCache_ = renderWorldMap(w, h, mapSrc_, ds_.mapDay, ds_.mapNight,
                                      ds_.showSolar, solSfi_, solA_, solK_);
        } else if (isShip) {
            // Same decode-once cache as the maps; a new picker choice
            // changes the key and reloads live.
            const QString key =
                QSettings().value("display/shipImagePath").toString();
            if (key != mapSrcKey_) {
                mapSrc_ = QImage(key).convertToFormat(QImage::Format_RGB32);
                mapSrcKey_ = key;
            }
            bgCache_ = renderShipImage(w, h, mapSrc_, ds_.mapDay);
        } else {
            bgCache_ = renderBlueRays(w, h);
        }
        bgMode_ = ds_.background;
        bgW_ = w;
        bgH_ = h;
        bgMinute_ = minute;
        bgDay_ = ds_.mapDay;
        bgNight_ = ds_.mapNight;
        bgSun_ = ds_.showSolar;
    }
    return bgCache_;
}

PanadapterWidget::PanadapterWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(640, 320);
    setMouseTracking(true);
    dbMax_ = ds_.refDb;
    dbMin_ = ds_.refDb - ds_.rangeDb;
    wfDbMax_ = ds_.wfRefDb;
    wfDbMin_ = ds_.wfRefDb - ds_.rangeDb;
}

void PanadapterWidget::setSpanHz(int spanHz) {
    fullSpanHz_ = std::max(1000, spanHz);
    viewSpanHz_ = std::clamp(viewSpanHz_, kMinViewSpanHz, maxViewSpanHz());
    update();
}

int PanadapterWidget::minViewSpanHz() const { return kMinViewSpanHz; }

void PanadapterWidget::setDialOffsetHz(int hz) {
    if (hz == dialOffsetHz_) return;
    dialOffsetHz_ = hz;
    clampViewShift();
    viewSpanHz_ = std::clamp(viewSpanHz_, kMinViewSpanHz, maxViewSpanHz());
    wfDirty_ = true;
    update();
}

void PanadapterWidget::setViewSpanHz(int spanHz) {
    // Slider-driven zoom: no viewSpanChanged emit, or the slider would loop.
    // PowerSDR CTUN rule: zooming IN first centers the view on the VFO (the
    // signal of interest is what's being zoomed on); zooming out never does.
    if (spanHz < viewSpanHz_ && viewShiftHz_ != 0) viewShiftHz_ = 0;
    viewSpanHz_ = std::clamp(spanHz, kMinViewSpanHz, maxViewSpanHz());
    clampViewShift();
    update();
}

void PanadapterWidget::setViewShiftHz(int hz) {
    if (hz == viewShiftHz_) return;
    viewShiftHz_ = hz;
    clampViewShift();
    wfDirty_ = true;
    update();
}

void PanadapterWidget::clampViewShift() {
    // The view (centered at dial + viewShift) must stay inside the capture
    // (centered at LO = dial + dialOffset): |shift - dialOffset| bounded by
    // the capture margin left over after the view span. The classic
    // dial-centered view (shift 0) is always left alone — the paint-time
    // bin clamp covers any degenerate startup geometry.
    if (viewShiftHz_ == 0) return;
    const int lim = std::max(0, (fullSpanHz_ - viewSpanHz_) / 2);
    viewShiftHz_ =
        std::clamp(viewShiftHz_, dialOffsetHz_ - lim, dialOffsetHz_ + lim);
}

void PanadapterWidget::setCenterHz(uint64_t hz) {
    if (hz == centerHz_) return;
    centerHz_ = hz;
    update();
}

namespace {
// Great-circle bearing (deg from north) and distance (km) between two points.
void greatCircle(double lat1d, double lon1d, double lat2d, double lon2d,
                 double& bearingDeg, double& distKm) {
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

// Azimuthal-equidistant world disc centered on the QTH (Thetis rose look):
// up = north, edge = the antipode, so a straight line from center IS the
// great-circle path at that bearing. Day/night tinted from the current sun.
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
} // namespace

void PanadapterWidget::setQth(double latDeg, double lonDeg) {
    qthLat_ = latDeg;
    qthLon_ = lonDeg;
    roseKey_.clear();                              // disc re-projects
    update();
}

void PanadapterWidget::pointRoseAt(double lat, double lon, const QString& label) {
    greatCircle(qthLat_, qthLon_, lat, lon, roseBearing_, roseDistKm_);
    roseLabel_ = label;
    emit roseBearingChanged(roseBearing_, roseLabel_);
    update();
}

void PanadapterWidget::setRotorAz(double azDeg) {
    if (azDeg == rotorAz_) return;
    rotorAz_ = azDeg;
    update();
}

bool PanadapterWidget::overRose(int x, int y) const {
    if (!ds_.showRose) return false;
    const int hSpec = spectrumHeight();
    if (hSpec < 2 * kRoseR + 30) return false;
    const int cx = 10 + kRoseR, cy = hSpec - kRoseR - 10;
    return std::hypot(double(x - cx), double(y - cy)) <= kRoseR;
}

void PanadapterWidget::drawCompassRose(QPainter& p, int hSpec) {
    if (!ds_.showRose || hSpec < 2 * kRoseR + 30) return;
    const int R = kRoseR;
    const int cx = 10 + R, cy = hSpec - R - 10;
    const qint64 minute = QDateTime::currentSecsSinceEpoch() / 60;
    const QString key = QString("%1|%2|%3").arg(R).arg(qthLat_).arg(qthLon_);
    if (roseKey_ != key || roseMinute_ != minute) {
        roseCache_ = renderRoseDisc(R, qthLat_, qthLon_);
        roseKey_ = key;
        roseMinute_ = minute;
    }
    p.setRenderHint(QPainter::Antialiasing);
    p.drawImage(cx - R, cy - R, roseCache_);
    // Ring + ticks every 30 degrees, cardinal labels.
    p.setPen(QPen(QColor(70, 84, 100), 2));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QPoint(cx, cy), R, R);
    QFont f = p.font();
    f.setPixelSize(9);
    f.setBold(true);
    p.setFont(f);
    for (int a = 0; a < 360; a += 30) {
        const double rad = a * M_PI / 180.0;
        const double sx = std::sin(rad), sy = -std::cos(rad);
        const bool cardinal = a % 90 == 0;
        p.setPen(QPen(QColor(150, 165, 182), cardinal ? 2 : 1));
        p.drawLine(QPointF(cx + sx * (R - (cardinal ? 9 : 5)),
                           cy + sy * (R - (cardinal ? 9 : 5))),
                   QPointF(cx + sx * R, cy + sy * R));
    }
    p.setPen(QColor(200, 212, 224));
    static const char* card[] = {"N", "E", "S", "W"};
    for (int i = 0; i < 4; ++i) {
        const double rad = i * 90.0 * M_PI / 180.0;
        const QPointF pt(cx + std::sin(rad) * (R - 17),
                         cy - std::cos(rad) * (R - 17));
        p.drawText(QRectF(pt.x() - 7, pt.y() - 7, 14, 14), Qt::AlignCenter,
                   card[i]);
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
        p.drawEllipse(QPointF(cx + std::sin(rad) * (R - 4),
                              cy - std::cos(rad) * (R - 4)), 3, 3);
    }
    // Rotor needle (actual antenna heading): cyan, under the red target
    // pointer so a converged rotor reads as one needle with a cyan halo.
    if (rotorAz_ >= 0.0) {
        const double rad = rotorAz_ * M_PI / 180.0;
        const QPointF tip(cx + std::sin(rad) * (R - 6),
                          cy - std::cos(rad) * (R - 6));
        p.setPen(QPen(QColor(80, 200, 235), 3.2, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(cx, cy), tip);
    }
    // Bearing pointer + readout.
    if (roseBearing_ >= 0.0) {
        const double rad = roseBearing_ * M_PI / 180.0;
        const QPointF tip(cx + std::sin(rad) * (R - 10),
                          cy - std::cos(rad) * (R - 10));
        p.setPen(QPen(QColor(235, 80, 60), 2.4, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(cx, cy), tip);
        p.setBrush(QColor(235, 80, 60));
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(cx, cy), 3, 3);
        // Readout box to the right of the disc.
        QFont rf = p.font();
        rf.setPixelSize(10);
        rf.setBold(true);
        p.setFont(rf);
        const QFontMetrics fm(rf);
        const QString l1 = roseLabel_.isEmpty() ? QString("MAN") : roseLabel_;
        const QString l2 = roseDistKm_ >= 0.0
            ? QString("%1°  %2 km").arg(qRound(roseBearing_))
                                   .arg(qRound(roseDistKm_))
            : QString("%1°").arg(qRound(roseBearing_));
        const int wBox = std::max(fm.horizontalAdvance(l1),
                                  fm.horizontalAdvance(l2)) + 14;
        const int lineH = fm.height() + 1;
        const QRect box(cx + R + 6, cy - lineH - 4, wBox, 2 * lineH + 8);
        p.fillRect(box, QColor(10, 14, 20, 175));
        p.setPen(QColor(235, 120, 90));
        p.drawText(box.left() + 7, box.top() + 4 + fm.ascent(), l1);
        p.setPen(QColor(220, 230, 240));
        p.drawText(box.left() + 7, box.top() + 4 + lineH + fm.ascent(), l2);
    }
}

void PanadapterWidget::setSolarInfo(int sfi, int aIdx, double kIdx, int ssn,
                                    const QString& xray) {
    solSfi_ = sfi;
    solA_   = aIdx;
    solK_   = kIdx;
    solSsn_ = ssn;
    solXray_ = xray;
    bgMinute_ = -1;                                // sun caption re-renders
    update();
}

// Thetis-style green space-weather block, bottom-right of the spectrum,
// over the trace floor where it doesn't fight signals or spot labels.
void PanadapterWidget::drawSolarPanel(QPainter& p, int hSpec) {
    if (!ds_.showSolar || solSfi_ < 0 || hSpec < 90) return;
    QFont f = p.font();
    f.setPixelSize(10);
    f.setBold(true);
    p.setFont(f);
    const QFontMetrics fm(f);
    QStringList lines;
    lines << QString("SFI %1   SSN %2").arg(solSfi_)
                 .arg(solSsn_ >= 0 ? QString::number(solSsn_) : QString("--"));
    lines << QString("A %1   K %2")
                 .arg(solA_ >= 0 ? QString::number(solA_) : QString("--"))
                 .arg(solK_ >= 0 ? QString::number(solK_, 'f', 1) : QString("--"));
    if (!solXray_.isEmpty()) lines << QString("X-RAY %1").arg(solXray_);
    // Rough HF-conditions verdict from the same numbers (flux feeds the
    // ionosphere, K wrecks it): worth a glance before calling CQ.
    QString cond;
    QColor  condC;
    if (solK_ >= 5.0 || (solSfi_ > 0 && solSfi_ < 75)) {
        cond = "COND POOR";
        condC = QColor(235, 80, 60);
    } else if (solSfi_ >= 110 && solK_ >= 0.0 && solK_ <= 3.0) {
        cond = "COND GOOD";
        condC = QColor(80, 230, 120);
    } else {
        cond = "COND FAIR";
        condC = QColor(240, 190, 60);
    }
    int wBox = fm.horizontalAdvance(cond);
    for (const QString& l : lines)
        wBox = std::max(wBox, fm.horizontalAdvance(l));
    wBox += 16;
    const int lineH = fm.height() + 1;
    const int hBox = (static_cast<int>(lines.size()) + 1) * lineH + 10;
    const QRect box(width() - wBox - 8, hSpec - hBox - 6, wBox, hBox);
    p.fillRect(box, QColor(4, 12, 6, 175));
    p.setPen(QColor(30, 90, 45));
    p.drawRect(box.adjusted(0, 0, -1, -1));
    p.setPen(QColor(80, 230, 120));
    int y = box.top() + 5 + fm.ascent();
    for (const QString& l : lines) {
        p.drawText(box.left() + 8, y, l);
        y += lineH;
    }
    p.setPen(condC);
    p.drawText(box.left() + 8, y, cond);
}

void PanadapterWidget::setSpots(const QVector<SpotLabel>& s) {
    spots_ = s;
    update();
}

void PanadapterWidget::setMarkers(const QVector<FreqMarker>& m) {
    markers_ = m;
    update();
}

// Pinned frequency markers (SDR-Console/Thetis style): a dashed amber line
// through the spectrum with its label flag at the top — visually distinct
// from spot markers (solid, colored by source) and from the notch. They
// never move or expire; that permanence is the point (skeds, nets, W1AW).
void PanadapterWidget::drawMarkers(QPainter& p, int hSpec) {
    if (markers_.isEmpty() || centerHz_ == 0 || hSpec < 40) return;
    QFont f = p.font();
    f.setPixelSize(11);
    f.setBold(true);
    p.setFont(f);
    const QFontMetrics fm(f);
    const qint64 c = static_cast<qint64>(centerHz_);
    for (const auto& m : markers_) {
        const qint64 off = m.hz - c;
        if (std::llabs(off - viewShiftHz_) > viewSpanHz_ / 2) continue;
        const int x = hzToX(static_cast<int>(off));
        QPen pen(QColor(255, 176, 64, 200), 1, Qt::DashLine);
        p.setPen(pen);
        p.drawLine(x, 14, x, hSpec - 2);
        const QString text = m.label.isEmpty()
            ? QString("%1").arg(m.hz / 1000.0, 0, 'f', 1)
            : m.label;
        const int tw = fm.horizontalAdvance(text);
        QRect box(x - tw / 2 - 4, 1, tw + 8, 13);
        box.moveLeft(std::clamp(box.left(), 0, width() - box.width()));
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(60, 42, 12, 215));
        p.drawRoundedRect(box, 2, 2);
        p.setPen(QColor(255, 190, 90));
        p.drawText(box, Qt::AlignCenter, text);
    }
    p.setBrush(Qt::NoBrush);
}

// Hover cursor (PowerSDR crosshair idea, minus the useless horizontal
// half — the Apache forums asked for exactly this): a dashed line under
// the mouse across spectrum AND waterfall, the frequency at the cursor
// riding it, and — the part PowerSDR doesn't have — a bright tick where
// a CW-zap click would actually land (live snap preview), so the
// operator sees BEFORE clicking whether the zap grabs the intended
// signal or its louder neighbor.
void PanadapterWidget::drawCursor(QPainter& p, int hSpec) {
    if (!ds_.showCursor || hoverX_ < 0 || drag_ != Drag::None
        || centerHz_ == 0)
        return;
    const int offHz = xToHz(hoverX_);
    p.setPen(QPen(QColor(150, 205, 235, 130), 1, Qt::DashLine));
    p.drawLine(hoverX_, 14, hoverX_, height());
    // frequency at the cursor, scale-format (7.028.4)
    const qint64 f = static_cast<qint64>(centerHz_) + offHz;
    const QString txt = QString("%1.%2.%3")
        .arg(f / 1000000)
        .arg((f / 1000) % 1000, 3, 10, QChar('0'))
        .arg((f % 1000) / 100);
    QFont fo = p.font(); fo.setPixelSize(10); fo.setBold(true); p.setFont(fo);
    const QFontMetrics fm(fo);
    const int tw = fm.horizontalAdvance(txt);
    const bool leftSide = hoverX_ + 10 + tw > width();
    const QRect box(leftSide ? hoverX_ - tw - 12 : hoverX_ + 6, 16,
                    tw + 6, 14);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(10, 16, 24, 200));
    p.drawRect(box);
    p.setPen(QColor(235, 216, 100));
    p.drawText(box, Qt::AlignCenter, txt);
    // zap snap preview: where the click would actually land
    if (snapFn_) {
        const int snapped = snapFn_(offHz);
        if (snapped != INT_MIN) {
            const int sx = hzToX(snapped);
            p.setPen(QPen(QColor(120, 255, 170, 220), 2));
            p.drawLine(sx, 34, sx, 34 + 16);
            if (std::abs(sx - hoverX_) > 3)        // caret when it differs
                p.drawLine(hoverX_, 42, sx, 42);
        }
    }
}

// UTC timestamps down the waterfall's left edge, one every ~48 px: history
// depth at a glance, and a scroll-back capture ("who called at :05?") reads
// like a log. Uses each ROW's recorded time, so speed changes and stalls
// stay honest instead of assuming a constant scroll rate.
void PanadapterWidget::drawWfTimes(QPainter& p, int wfTop) {
    const int wfH = height() - wfTop;
    if (!ds_.showWfTime || wfH < 40 || wfTimes_.empty() || wfCount_ < 2)
        return;
    QFont f = p.font();
    f.setPixelSize(10);
    f.setBold(false);
    p.setFont(f);
    for (int y = 12; y < std::min(wfH, wfCount_); y += 48) {
        const qint64 t = wfTimes_[static_cast<size_t>(
            ((wfHead_ - 1 - y) % kWfHistRows + kWfHistRows) % kWfHistRows)];
        if (t <= 0) continue;
        const QString s =
            QDateTime::fromMSecsSinceEpoch(t).toUTC().toString("hh:mm:ss");
        p.setPen(QColor(220, 230, 240, 130));
        p.drawLine(0, wfTop + y, 3, wfTop + y);
        p.setPen(QColor(0, 0, 0, 170));
        p.drawText(7, wfTop + y + 4, s);           // soft shadow
        p.setPen(QColor(215, 225, 235, 175));
        p.drawText(6, wfTop + y + 3, s);
    }
}

void PanadapterWidget::setCallsign(const QString& call) {
    callsign_ = call;
    update();
}

void PanadapterWidget::setVfoBVisible(bool on) {
    vfoBVisible_ = on;
    update();
}

void PanadapterWidget::setVfoLocks(bool a, bool b) {
    lockA_ = a;
    lockB_ = b;
}

void PanadapterWidget::setVfoB(uint64_t hz, char role, int loHz, int hiHz) {
    if (drag_ == Drag::VfoB || drag_ == Drag::BLoEdge || drag_ == Drag::BHiEdge
        || drag_ == Drag::BSymEdge) return;        // don't snap it mid-drag
    if (hz == vfoBHz_ && role == vfoBRole_ && loHz == vfoBLo_ && hiHz == vfoBHi_)
        return;
    vfoBHz_ = hz;
    vfoBRole_ = role;
    vfoBLo_ = loHz;
    vfoBHi_ = hiHz;
    update();
}

bool PanadapterWidget::overVfoB(int x) const {
    if (!vfoBVisible_ || !vfoBHz_ || !centerHz_) return false;
    const qint64 off = static_cast<qint64>(vfoBHz_) - static_cast<qint64>(centerHz_);
    if (std::llabs(off) > viewSpanHz_) return false;
    const int xb  = hzToX(static_cast<int>(off));
    const int xLo = hzToX(static_cast<int>(off) + vfoBLo_);
    const int xHi = hzToX(static_cast<int>(off) + vfoBHi_);
    return x >= std::min(xLo, xb) - 4 && x <= std::max(xHi, xb) + 4;
}

// KE9NS-style spot markers, horizontal-stack flavor (his spotter as of
// v2.8.0.330: horizontal calls on dark boxes since .261, vertical indicator
// optional since .269 and shortened to half height in .300). A short low-key
// line marks the exact frequency; the callsign is drawn HORIZONTALLY on a
// dark box, packed into the topmost row where it doesn't cover another call —
// readable at contest density, where rotated labels turn into a picket
// fence. Labels sit right of the marker on USB bands and left on LSB
// (160/80/40) so the signal itself stays clear. Left-click tunes there,
// right-click looks the call up on QRZ; fresh spots draw brighter than ones
// about to expire.
void PanadapterWidget::drawSpots(QPainter& p, int hSpec) {
    spotHits_.clear();
    if (spots_.isEmpty() || centerHz_ == 0 || hSpec < 60) return;
    QFont f = p.font();
    f.setPixelSize(10);
    f.setBold(true);
    p.setFont(f);
    const QFontMetrics fm(f);
    const qint64 half = viewSpanHz_ / 2;
    struct Vis { int x; const SpotLabel* s; bool lsb; };
    std::vector<Vis> vis;
    for (const SpotLabel& s : spots_) {
        const qint64 off = s.hz - static_cast<qint64>(centerHz_);
        if (off < viewShiftHz_ - half || off > viewShiftHz_ + half) continue;
        const bool lsb = s.hz < 5250000                  // voice below 10 MHz is
                      || (s.hz > 6000000 && s.hz < 8000000); // LSB (60 m excepted)
        vis.push_back({hzToX(static_cast<int>(off)), &s, lsb});
    }
    if (vis.empty()) return;
    std::sort(vis.begin(), vis.end(),
              [](const Vis& a, const Vis& b) { return a.x < b.x; });
    const int topY = 18;
    const int rowH = fm.height() + 2;
    const int maxRows = std::clamp((hSpec / 2 - topY) / rowH, 1, 8);
    // Marker lines first so every label draws over them (KE9NS draw order).
    p.setPen(QColor(150, 120, 255, 80));
    const int lineBot = std::min(topY + maxRows * rowH + 8, hSpec);
    for (const Vis& v : vis) p.drawLine(v.x, topY, v.x, lineBot);
    // Pack each label into the topmost row with room at its x. The input is
    // x-sorted, so per row it's enough to remember the rightmost occupied
    // edge; sorting also keeps row assignments stable frame to frame. When a
    // pileup fills every row at some x, further calls there are dropped
    // (their marker line still shows) instead of smearing over each other.
    std::vector<int> rowEdge(maxRows, INT_MIN);
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    for (const Vis& v : vis) {
        const int wCall = fm.horizontalAdvance(v.s->call);
        const int wTag  = v.s->tag.isEmpty() ? 0
                        : fm.horizontalAdvance(v.s->tag) + 4;
        const int w = wCall + wTag;
        int xt = v.lsb ? v.x - w - 5 : v.x + 5;
        xt = std::clamp(xt, 2, std::max(2, width() - w - 2));
        int row = -1;
        for (int r = 0; r < maxRows; ++r)
            if (xt - 4 > rowEdge[r]) { row = r; break; }
        if (row < 0) continue;
        rowEdge[row] = xt + w;
        const QRect box(xt - 3, topY + row * rowH, w + 6, rowH - 1);
        // Fade with age toward expiry (unknown age = fresh).
        const qint64 age = v.s->atSecs > 0
            ? std::clamp<qint64>(now - v.s->atSecs, 0, 1200) : 0;
        const int alpha = 255 - static_cast<int>(age * 115 / 1200);
        const QColor kindC = v.s->kind == 'P' ? QColor(74, 222, 128)  // POTA
                           : v.s->kind == 'F' ? QColor(96, 200, 255) // FT8
                           : v.s->kind == 'S' ? QColor(205, 140, 255)// skimmer
                                              : QColor(255, 216, 50);// DX
        // Worked-before coloring (cqrlog DX-cluster convention): the CALL
        // text carries the logbook status; the source kind stays readable
        // as a small left edge bar in the kind color.
        QColor c = v.s->status == 'N' ? QColor(255, 92, 70)   // new one!
                 : v.s->status == 'B' ? QColor(255, 190, 60)  // new band
                 : v.s->status == 'W' ? QColor(130, 222, 140) // worked
                 : v.s->status == 'C' ? QColor(150, 162, 178) // confirmed
                                      : kindC;                // no log data
        c.setAlpha(alpha);
        p.fillRect(box, QColor(8, 10, 16, 185));         // dark box (.261 idea)
        if (v.s->alert) {                                // DX-watch hit
            p.setPen(QPen(QColor(255, 150, 40, alpha), 1));
            p.drawRect(box.adjusted(0, 0, -1, -1));
        }
        if (v.s->status != '?') {                        // kind edge bar
            QColor kc = kindC;
            kc.setAlpha(alpha);
            p.fillRect(QRect(box.left(), box.top(), 2, box.height()), kc);
        }
        p.setPen(c);
        p.drawText(xt, box.top() + fm.ascent(), v.s->call);
        if (wTag) {                                      // park ref, muted gray
            p.setPen(QColor(150, 162, 178, alpha));
            p.drawText(xt + wCall + 4, box.top() + fm.ascent(), v.s->tag);
        }
        spotHits_.push_back({box, v.s->hz, v.s->call, v.s->lat, v.s->lon,
                             v.s->kind, v.s->tag});
    }
}

void PanadapterWidget::setDisplaySettings(const DisplaySettings& s) {
    const bool scaleChanged = s.refDb != ds_.refDb || s.rangeDb != ds_.rangeDb
                              || s.palette != ds_.palette;
    if (s.peakHold && !ds_.peakHold) peaks_ = avg_;   // fresh start, no stale peaks
    ds_ = s;
    ds_.avgFrames = std::max(1, ds_.avgFrames);
    ds_.wfSpeed   = std::max(1, ds_.wfSpeed);
    ds_.split     = std::clamp(ds_.split, 0.15f, 0.85f);
    dbMax_ = ds_.refDb;
    dbMin_ = ds_.refDb - std::max(10.0f, ds_.rangeDb);
    wfDbMax_ = ds_.wfRefDb;
    wfDbMin_ = ds_.wfRefDb - std::max(10.0f, ds_.rangeDb);
    if (scaleChanged)
        wfDirty_ = true;       // history re-renders in the new scale/palette
    update();
}

void PanadapterWidget::setPassband(int loHz, int hiHz) {
    if (drag_ != Drag::None) return;               // never fight an active edge drag
    pbLoHz_ = std::min(loHz, hiHz);
    pbHiHz_ = std::max(loHz, hiHz);
    update();
}

void PanadapterWidget::setBandZones(const QVector<BandZone>& zones) {
    zones_ = zones;
    update();
}

void PanadapterWidget::setBwAnchor(int a) {
    bwAnchor_ = a < 0 ? -1 : (a > 0 ? 1 : 0);
}

void PanadapterWidget::setNotch(bool on, int rfOffsetHz, int widthHz, bool peak) {
    if (drag_ == Drag::Notch) return;              // never fight an active drag
    notchOn_ = on;
    notchPeak_ = peak;
    notchRfHz_ = rfOffsetHz;
    notchWidthHz_ = widthHz;
    update();
}

bool PanadapterWidget::overNotch(int x) const {
    if (!notchOn_) return false;
    const int halfPx = (hzToX(notchRfHz_ + notchWidthHz_ / 2)
                        - hzToX(notchRfHz_ - notchWidthHz_ / 2)) / 2;
    return std::abs(x - hzToX(notchRfHz_)) <= std::max(6, halfPx + 3);
}

int PanadapterWidget::spectrumHeight() const {
    return static_cast<int>((height() - kScaleBandH) * ds_.split);
}

bool PanadapterWidget::inScaleBand(int y) const {
    const int hSpec = spectrumHeight();
    return y >= hSpec - 2 && y < hSpec + kScaleBandH + 2;
}

bool PanadapterWidget::inDbAxis(int x, int y) const {
    return x < kDbAxisW && y < spectrumHeight() - 2;
}

bool PanadapterWidget::inWfDbAxis(int x, int y) const {
    return x < kDbAxisW && y > spectrumHeight() + kScaleBandH + 2;
}

QRgb PanadapterWidget::mapColor(float t) const {
    const Palette& pal =
        kPalettes[std::clamp(ds_.palette, 0, int(std::size(kPalettes)) - 1)];
    t = std::clamp(t, 0.0f, 1.0f);
    for (size_t i = 1; i < pal.n; ++i) {
        if (t <= pal.stops[i].t) {
            const Stop& a = pal.stops[i - 1];
            const Stop& b = pal.stops[i];
            const float f = (t - a.t) / (b.t - a.t);
            return qRgb(int(a.r + f * (b.r - a.r)),
                        int(a.g + f * (b.g - a.g)),
                        int(a.b + f * (b.b - a.b)));
        }
    }
    return qRgb(255, 255, 255);
}

void PanadapterWidget::pushWaterfallRow(const std::vector<float>& db) {
    const int n = static_cast<int>(db.size());
    if (n < 2) return;
    if (wfBins_ != n) {                            // bin-count change: reset history
        wfBins_ = n;
        wfHist_.assign(static_cast<size_t>(kWfHistRows) * n, kNoRow);
        wfTimes_.assign(kWfHistRows, 0);
        wfHead_ = wfCount_ = wfPending_ = 0;
        wfDirty_ = true;
    }
    std::memcpy(&wfHist_[static_cast<size_t>(wfHead_) * n], db.data(),
                sizeof(float) * n);
    wfTimes_[static_cast<size_t>(wfHead_)] = QDateTime::currentMSecsSinceEpoch();
    wfHead_ = (wfHead_ + 1) % kWfHistRows;
    wfCount_ = std::min(wfCount_ + 1, kWfHistRows);
    ++wfPending_;
}

// Crisp column mapping: each pixel column takes the MAX of its bin range, so
// narrow signals never vanish on wide spans and nothing is smoothed away.
void PanadapterWidget::renderWfLine(const float* src, QRgb* dst, int w,
                                    int binLo, int binHi) const {
    const int nv = binHi - binLo;
    const float invRange = 1.0f / (wfDbMax_ - wfDbMin_);
    for (int j = 0; j < w; ++j) {
        int b0 = binLo + static_cast<int>(static_cast<int64_t>(j) * nv / w);
        int b1 = binLo + static_cast<int>(static_cast<int64_t>(j + 1) * nv / w);
        if (b1 <= b0) b1 = b0 + 1;
        float m = src[b0];
        for (int b = b0 + 1; b < b1; ++b) m = std::max(m, src[b]);
        dst[j] = mapColor((m - wfDbMin_) * invRange);
    }
}

void PanadapterWidget::ensureWaterfallImage(int w, int h, int binLo, int binHi) {
    if (w < 1 || h < 1 || wfBins_ < 2) return;
    const bool geomChanged = wfImg_.width() != w || wfImg_.height() != h
                             || binLo != lastBinLo_ || binHi != lastBinHi_;
    auto histRow = [this](int back) {
        return &wfHist_[static_cast<size_t>(
            ((wfHead_ - 1 - back) % kWfHistRows + kWfHistRows) % kWfHistRows) * wfBins_];
    };
    if (geomChanged || wfDirty_) {                 // full re-render from history
        wfImg_ = QImage(w, h, QImage::Format_RGB32);
        for (int y = 0; y < h; ++y) {
            QRgb* line = reinterpret_cast<QRgb*>(wfImg_.scanLine(y));
            if (y < wfCount_) renderWfLine(histRow(y), line, w, binLo, binHi);
            else              std::fill(line, line + w, qRgb(0, 0, 0));
        }
        lastBinLo_ = binLo;
        lastBinHi_ = binHi;
        wfDirty_ = false;
    } else if (wfPending_ > 0) {                   // scroll + render only new rows
        const int rows = std::min(wfPending_, h);
        const int bpl = wfImg_.bytesPerLine();
        std::memmove(wfImg_.bits() + static_cast<size_t>(bpl) * rows, wfImg_.bits(),
                     static_cast<size_t>(bpl) * (h - rows));
        for (int y = 0; y < rows; ++y)
            renderWfLine(histRow(y), reinterpret_cast<QRgb*>(wfImg_.scanLine(y)),
                         w, binLo, binHi);
    }
    wfPending_ = 0;
}

void PanadapterWidget::setSpectrum(const std::vector<float>& magsDb) {
    const size_t n = magsDb.size();
    spectrum_ = magsDb;
    if (avg_.size() != n) {                        // bin count changed: hard reset
        avg_ = magsDb;
        peaks_ = magsDb;
    } else {
        const float a = 1.0f / ds_.avgFrames;      // EMA; avgFrames==1 -> raw
        for (size_t i = 0; i < n; ++i) {
            avg_[i] += (magsDb[i] - avg_[i]) * a;
            if (ds_.peakHold)
                peaks_[i] = std::max(peaks_[i] - kPeakDecayDb, avg_[i]);
        }
    }
    // Waterfall speed: max-merge wfSpeed frames into one committed row, so slow
    // scroll keeps every blip instead of dropping frames.
    if (wfAccum_.size() != n) {
        wfAccum_ = magsDb;
        wfAccumN_ = 1;
    } else {
        for (size_t i = 0; i < n; ++i) wfAccum_[i] = std::max(wfAccum_[i], magsDb[i]);
        ++wfAccumN_;
    }
    if (wfAccumN_ >= ds_.wfSpeed) {
        pushWaterfallRow(wfAccum_);
        std::fill(wfAccum_.begin(), wfAccum_.end(), kNoRow);
        wfAccumN_ = 0;
    }
    update();
}

int PanadapterWidget::hzToX(int hz) const {
    // hz is an RF offset from the DIAL; the screen is centered on the view
    // center (dial + viewShift). With shift 0 this is the classic mapping.
    const double frac =
        (hz - viewShiftHz_ + viewSpanHz_ / 2.0) / viewSpanHz_;
    return static_cast<int>(std::lround(frac * width()));
}

int PanadapterWidget::xToHz(int x) const {
    const double frac = static_cast<double>(x) / std::max(1, width());
    return static_cast<int>(
        std::lround(frac * viewSpanHz_ - viewSpanHz_ / 2.0)) + viewShiftHz_;
}

// Regulatory zone boxes (KE9NS BandText style): translucent green segments
// with a label riding the top edge, drawn wherever an allocation falls inside
// the view. Spectrum area only, under the trace and passband tints.
void PanadapterWidget::drawBandZones(QPainter& p, int hSpec) {
    if (zones_.isEmpty() || !centerHz_) return;
    QFont f = p.font();
    f.setPixelSize(10);
    p.setFont(f);
    const qint64 c = static_cast<qint64>(centerHz_);
    const qint64 vLo = viewShiftHz_ - viewSpanHz_ / 2;
    const qint64 vHi = viewShiftHz_ + viewSpanHz_ / 2;
    for (const auto& z : zones_) {
        const qint64 lo = z.loHz - c, hi = z.hiHz - c;
        if (hi < vLo || lo > vHi) continue;
        const int xLo = hzToX(static_cast<int>(std::max(lo, vLo)));
        const int xHi = hzToX(static_cast<int>(std::min(hi, vHi)));
        p.fillRect(QRect(QPoint(xLo, 0), QPoint(xHi, hSpec)), QColor(60, 190, 90, 22));
        p.setPen(QPen(QColor(80, 210, 110, 130), 1));
        p.drawLine(xLo, 0, xLo, hSpec);
        p.drawLine(xHi, 0, xHi, hSpec);
        p.drawLine(xLo, 1, xHi, 1);
        p.setPen(QColor(140, 235, 160, 200));
        p.drawText(QRect(xLo + 3, 3, std::max(10, xHi - xLo - 5), 13),
                   Qt::AlignLeft | Qt::AlignTop, z.label);
    }
}

void PanadapterWidget::drawFreqGrid(QPainter& p, int hSpec) {
    if (!ds_.showGrid) return;
    if (centerHz_ == 0) {                          // no dial info: plain grid only
        p.setPen(QColor(255, 255, 255, 22));
        for (int i = 1; i < 10; ++i)
            p.drawLine(width() * i / 10, 0, width() * i / 10, hSpec);
        return;
    }
    // Gridlines on absolute "nice" frequencies (labels live on the scale band).
    // KE9NS density: fainter minor lines at the scale band's minor-tick step
    // between the major lines, so the grid reads every 1/5 of a label step.
    const double step = niceStep(viewSpanHz_);
    const double f0 =
        static_cast<double>(centerHz_) + viewShiftHz_ - viewSpanHz_ / 2.0;
    const double f1 =
        static_cast<double>(centerHz_) + viewShiftHz_ + viewSpanHz_ / 2.0;
    const double minor = step / 5.0;
    if (hzToX(static_cast<int>(minor)) - hzToX(0) > 14) {
        p.setPen(QColor(255, 255, 255, 14));
        for (double fq = std::ceil(f0 / minor) * minor; fq <= f1; fq += minor) {
            const qint64 q = static_cast<qint64>(std::llround(fq));
            if (q % static_cast<qint64>(std::llround(step)) == 0) continue;
            const int x = hzToX(static_cast<int>(std::lround(fq - double(centerHz_))));
            p.drawLine(x, 0, x, hSpec);
        }
    }
    p.setPen(QColor(255, 255, 255, 34));
    for (double fq = std::ceil(f0 / step) * step; fq <= f1; fq += step) {
        const int x = hzToX(static_cast<int>(std::lround(fq - double(centerHz_))));
        p.drawLine(x, 0, x, hSpec);
    }
}

// KE9NS/PowerSDR-style frequency scale: a dedicated strip between the spectrum
// and the waterfall with ticks + MHz labels. The strip is also the split
// handle — drag it up/down to resize spectrum vs waterfall.
namespace {
// US band-plan segments for the scale-strip tint, colored to the ARRL
// "US Amateur Radio Bands" chart: RTTY/data = red, phone/image = green,
// CW-only = blue, Novice/Tech SSB = amber, 60 m USB channels = cyan.
enum class PMode { Data, Phone, Cw, SsbNT, Usb60 };
struct PlanSeg { qint64 lo, hi; PMode mode; };
constexpr PlanSeg kUsPlan[] = {
    {1800000, 2000000, PMode::Phone},                                 // 160
    {3500000, 3600000, PMode::Data},   {3600000, 4000000, PMode::Phone}, // 80/75
    {5330500, 5406400, PMode::Usb60},                                 // 60 (channels)
    {7000000, 7125000, PMode::Data},   {7125000, 7300000, PMode::Phone}, // 40
    {10100000, 10150000, PMode::Data},                                // 30 (no phone)
    {14000000, 14150000, PMode::Data}, {14150000, 14350000, PMode::Phone}, // 20
    {18068000, 18110000, PMode::Data}, {18110000, 18168000, PMode::Phone}, // 17
    {21000000, 21200000, PMode::Data}, {21200000, 21450000, PMode::Phone}, // 15
    {24890000, 24930000, PMode::Data}, {24930000, 24990000, PMode::Phone}, // 12
    {28000000, 28300000, PMode::Data}, {28300000, 29700000, PMode::Phone}, // 10
    {50000000, 50100000, PMode::Cw},   {50100000, 54000000, PMode::Phone}, // 6
};
// ARRL chart colors (opaque; a translucent copy tints the strip).
inline QColor planColor(PMode m) {
    switch (m) {
        case PMode::Data:  return QColor(214,  67,  67);   // RTTY and data (red)
        case PMode::Phone: return QColor( 58, 182, 105);   // phone and image (green)
        case PMode::Cw:    return QColor( 96, 152, 236);   // CW only (blue)
        case PMode::SsbNT: return QColor(232, 196,  66);   // Novice/Tech SSB (amber)
        case PMode::Usb60: return QColor( 78, 200, 230);   // 60 m USB (cyan)
    }
    return QColor(120, 120, 120);
}
// US license-class privilege edges: the frequency where a class first gains
// PHONE privileges on that band (from the ARRL chart's E/A/G/T markers).
// Drawn as guide lines "in the map" so the operator sees who can work where.
struct PrivEdge { qint64 hz; const char* cls; };
constexpr PrivEdge kUsPriv[] = {
    {3600000, "E"},  {3700000, "A"},  {3800000, "G"},         // 75 m phone
    {7125000, "E/A"},{7175000, "G"},                          // 40 m phone
    {14150000,"E"},  {14175000,"A"},  {14225000,"G"},         // 20 m phone
    {21200000,"E"},  {21225000,"A"},  {21275000,"G"},         // 15 m phone
    {28300000,"G+"}, {28500000,"N/T"},                        // 10 m phone / N-T SSB top
    {50100000,"all"},                                         // 6 m phone
};
} // namespace

void PanadapterWidget::drawPrivileges(QPainter& p, int hSpec) {
    // Faint vertical guides across the spectrum at US license-class phone
    // edges, each tagged with the class letter — so at a glance you can see
    // where an Extra / Advanced / General / Tech may transmit on this band.
    if (!ds_.showPrivileges || centerHz_ == 0) return;
    const qint64 half = viewSpanHz_ / 2;
    const qint64 vc = qint64(centerHz_) + viewShiftHz_;   // view center RF
    QFont f = p.font();
    f.setPixelSize(9);
    f.setBold(true);
    p.setFont(f);
    for (const PrivEdge& e : kUsPriv) {
        if (e.hz < vc - half || e.hz > vc + half)
            continue;
        const int x = hzToX(int(e.hz - qint64(centerHz_)));
        p.setPen(QPen(QColor(70, 190, 120, 90), 1, Qt::DashLine));
        p.drawLine(x, 12, x, hSpec);
        p.setPen(QColor(150, 235, 180, 220));
        p.drawText(x + 2, 11, QString::fromLatin1(e.cls));
    }
}

void PanadapterWidget::drawScaleBand(QPainter& p, int hSpec) {
    p.fillRect(QRect(0, hSpec, width(), kScaleBandH), QColor(20, 27, 36));
    // Band-plan tint under the ticks, ARRL colors (red data / green phone /
    // blue CW / amber N-T SSB / cyan 60 m). Bolder than before: a stronger
    // fill plus a full-opacity 2 px accent bar so the segments read at a
    // glance instead of a faint wash.
    if (ds_.showBandPlan && centerHz_ != 0) {
        const qint64 half = viewSpanHz_ / 2;
        const qint64 vLo = viewShiftHz_ - half, vHi = viewShiftHz_ + half;
        for (const PlanSeg& s : kUsPlan) {
            const qint64 lo = s.lo - qint64(centerHz_);
            const qint64 hi = s.hi - qint64(centerHz_);
            if (hi < vLo || lo > vHi)
                continue;
            const int x0 = std::max(0, hzToX(int(std::max(lo, vLo))));
            const int x1 = std::min(width(), hzToX(int(std::min(hi, vHi))));
            if (x1 <= x0) continue;
            QColor c = planColor(s.mode);
            c.setAlpha(95);                                    // strip body
            p.fillRect(QRect(x0, hSpec + 1, x1 - x0, kScaleBandH - 3), c);
            c.setAlpha(235);                                   // accent bar
            p.fillRect(QRect(x0, hSpec + kScaleBandH - 3, x1 - x0, 2), c);
        }
    }
    p.setPen(QColor(255, 255, 255, 50));
    p.drawLine(0, hSpec, width(), hSpec);
    p.drawLine(0, hSpec + kScaleBandH - 1, width(), hSpec + kScaleBandH - 1);
    if (centerHz_ == 0) return;
    const double step = niceStep(viewSpanHz_);
    const double f0 =
        static_cast<double>(centerHz_) + viewShiftHz_ - viewSpanHz_ / 2.0;
    const double f1 =
        static_cast<double>(centerHz_) + viewShiftHz_ + viewSpanHz_ / 2.0;
    QFont f = p.font();
    f.setPixelSize(10);
    f.setBold(true);
    p.setFont(f);
    // Minor ticks every step/5 when there's room for them.
    const double minor = step / 5.0;
    if (hzToX(static_cast<int>(minor)) - hzToX(0) > 8) {
        p.setPen(QColor(160, 175, 190, 110));
        for (double fq = std::ceil(f0 / minor) * minor; fq <= f1; fq += minor) {
            const int x = hzToX(static_cast<int>(std::lround(fq - double(centerHz_))));
            p.drawLine(x, hSpec, x, hSpec + 3);
        }
    }
    // KE9NS-style labels: yellow, MHz.kkk.h format (e.g. 7.280.0 — the last
    // digit is hundreds of Hz).
    for (double fq = std::ceil(f0 / step) * step; fq <= f1; fq += step) {
        const int x = hzToX(static_cast<int>(std::lround(fq - double(centerHz_))));
        p.setPen(QColor(200, 215, 230, 200));
        p.drawLine(x, hSpec, x, hSpec + 5);
        const qint64 hz  = static_cast<qint64>(std::llround(fq));
        const qint64 mhz = hz / 1000000;
        const int    khz = static_cast<int>((hz / 1000) % 1000);
        const int    hun = static_cast<int>((hz % 1000) / 100);
        p.setPen(QColor(255, 216, 50));
        p.drawText(x + 3, hSpec + kScaleBandH - 4,
                   QString("%1.%2.%3").arg(mhz)
                       .arg(khz, 3, 10, QLatin1Char('0')).arg(hun));
    }
}

void PanadapterWidget::drawDbScale(QPainter& p, int hSpec) {
    QFont f = p.font();
    f.setPixelSize(9);
    p.setFont(f);
    const float range = dbMax_ - dbMin_;
    const int step = range > 80.0f ? 20 : 10;      // KE9NS-density when zoomed in
    // Minor lines at half the label step (KE9NS grid density), unlabeled.
    if (ds_.showGrid) {
        p.setPen(QColor(255, 255, 255, 12));
        const int half = step / 2;
        for (int db = static_cast<int>(std::ceil(dbMin_ / half)) * half;
             db < static_cast<int>(dbMax_); db += half) {
            if (db % step == 0) continue;          // majors drawn below
            const int y = static_cast<int>(hSpec * (1.0f - (db - dbMin_) / range));
            if (y < 20 || y > hSpec - 6) continue;
            p.drawLine(0, y, width(), y);
        }
    }
    for (int db = static_cast<int>(std::ceil(dbMin_ / step)) * step;
         db < static_cast<int>(dbMax_); db += step) {
        const int y = static_cast<int>(hSpec * (1.0f - (db - dbMin_) / range));
        if (y < 20 || y > hSpec - 6) continue;     // keep clear of the text row
        if (ds_.showGrid) {                        // labels stay (axis is a control)
            p.setPen(QColor(255, 255, 255, 26));
            p.drawLine(0, y, width(), y);
        }
        p.setPen(QColor(255, 216, 50, 200));
        p.drawText(4, y - 2, QString::number(db));
    }
}

void PanadapterWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    const int hSpec = spectrumHeight();
    p.fillRect(rect(), QColor(12, 16, 22));
    if (ds_.background != 0 && hSpec > 1)          // KE9NS-style backdrop
        p.drawImage(0, 0, backgroundImage(width(), hSpec));

    // Station-callsign watermark: faint gray in the upper left (clear of the
    // span readout), under everything the display draws on top.
    if (ds_.showCall && !callsign_.isEmpty() && hSpec > 40) {
        const QFont saved = p.font();
        QFont f = saved;
        f.setPixelSize(std::min(hSpec / 6, width() / 14));
        f.setBold(true);
        f.setLetterSpacing(QFont::PercentageSpacing, 112);
        p.setFont(f);
        p.setPen(QColor(200, 215, 230, 26));
        p.drawText(QRect(kDbAxisW + 10, 20, width() - kDbAxisW - 20, hSpec - 24),
                   Qt::AlignLeft | Qt::AlignTop, callsign_);
        p.setFont(saved);
    }

    const int n = static_cast<int>(spectrum_.size());
    // Visible bin window: view span centered inside the full captured span.
    int binLo = 0, binHi = n;
    if (n >= 2) {
        // The view is centered on dial + viewShift; the dial itself sits
        // BELOW the capture center by dialOffsetHz_ (offset tuning), so the
        // window lands at dialOffset - viewShift below the capture center.
        const double frac = static_cast<double>(viewSpanHz_) / fullSpanHz_;
        const double viewFrac =
            0.5 - static_cast<double>(dialOffsetHz_ - viewShiftHz_) / fullSpanHz_;
        binLo = static_cast<int>(n * (viewFrac - frac / 2.0));
        binHi = static_cast<int>(n * (viewFrac + frac / 2.0));
        binLo = std::clamp(binLo, 0, n - 2);
        binHi = std::clamp(binHi, binLo + 2, n);
    }

    // Waterfall (below the scale band): raw history, 1 row = 1 pixel line.
    const int wfTop = hSpec + kScaleBandH;
    if (wfBins_ >= 2 && n >= 2 && height() > wfTop) {
        ensureWaterfallImage(width(), height() - wfTop, binLo, binHi);
        if (!wfImg_.isNull()) p.drawImage(0, wfTop, wfImg_);
        drawWfTimes(p, wfTop);
        if (std::abs(ds_.wfRefDb - ds_.refDb) > 0.5f) {
            QFont wf = p.font(); wf.setPixelSize(9); wf.setBold(true);
            p.setFont(wf);
            p.setPen(QColor(0, 0, 0, 160));
            p.drawText(kDbAxisW + 7, wfTop + 13,
                       QString("WF %1 dB").arg(qRound(ds_.wfRefDb)));
            p.setPen(QColor(200, 220, 240, 150));
            p.drawText(kDbAxisW + 6, wfTop + 12,
                       QString("WF %1 dB").arg(qRound(ds_.wfRefDb)));
        }
    }

    // Per-pixel-column signal level (0..1 of the scale): max of the column's
    // bins when zoomed out (narrow signals never vanish), linear interpolation
    // between bins when zoomed deep (no staircase). Drives both fill and trace,
    // and gives a clean one-vertex-per-pixel line instead of sub-pixel jitter.
    const float range = dbMax_ - dbMin_;
    const int w = width();
    std::vector<float> colT;
    if (n >= 2 && w > 1) {
        colT.resize(w);
        const int nv = binHi - binLo;
        for (int x = 0; x < w; ++x) {
            float v;
            if (nv >= w) {
                int b0 = binLo + static_cast<int>(static_cast<int64_t>(x) * nv / w);
                int b1 = binLo + static_cast<int>(static_cast<int64_t>(x + 1) * nv / w);
                if (b1 <= b0) b1 = b0 + 1;
                v = avg_[b0];
                for (int b = b0 + 1; b < b1; ++b) v = std::max(v, avg_[b]);
            } else {
                const double pos = binLo + static_cast<double>(x) * (nv - 1) / (w - 1);
                const int b = static_cast<int>(pos);
                const float f = static_cast<float>(pos - b);
                v = avg_[b] * (1.0f - f) + avg_[std::min(b + 1, n - 1)] * f;
            }
            colT[x] = std::clamp((v - dbMin_) / range, 0.0f, 1.0f);
        }
    }

    // KE9NS-style fill: color keyed to the dB AXIS, so the bands run
    // horizontally and a signal climbs through blue -> green -> yellow -> red
    // as it gets stronger (per-column coloring made vertical streaks instead).
    // Gamma-compressed so the warm colors arrive partway up the scale, not
    // only at the very top.
    if (!colT.empty() && ds_.fillTrace && hSpec > 1) {
        // Transparent above the trace so the backdrop (map/rays) shows through.
        if (fillImg_.width() != w || fillImg_.height() != hSpec
            || fillImg_.format() != QImage::Format_ARGB32_Premultiplied)
            fillImg_ = QImage(w, hSpec, QImage::Format_ARGB32_Premultiplied);
        std::vector<int> colY(w);
        for (int x = 0; x < w; ++x)
            colY[x] = static_cast<int>(hSpec * (1.0f - colT[x]));
        for (int y = 0; y < hSpec; ++y) {
            const float t = 1.0f - static_cast<float>(y) / (hSpec - 1);
            const QRgb c = mapColor(std::pow(t, kFillGamma));
            QRgb* line = reinterpret_cast<QRgb*>(fillImg_.scanLine(y));
            for (int x = 0; x < w; ++x)
                line[x] = (y >= colY[x]) ? c : qRgba(0, 0, 0, 0);
        }
        p.drawImage(0, 0, fillImg_);
    }

    // Frequency gridlines and the (draggable) dB scale, over the fill.
    drawFreqGrid(p, hSpec);
    drawDbScale(p, hSpec);
    drawBandZones(p, hSpec);

    QPainterPath tracePath;
    if (!colT.empty()) {
        for (int x = 0; x < w; ++x) {
            const double y = hSpec * (1.0 - colT[x]);
            if (x == 0) tracePath.moveTo(x, y);
            else        tracePath.lineTo(x, y);
        }
    }

    // Passband highlight across spectrum AND waterfall (SmartSDR-style).
    const int xLo = hzToX(pbLoHz_), xHi = hzToX(pbHiHz_);
    p.fillRect(QRect(QPoint(xLo, 0), QPoint(xHi, height())), QColor(60, 120, 200, 55));
    p.setPen(QPen(QColor(120, 180, 255), 1));
    p.drawLine(xLo, 0, xLo, height());
    p.drawLine(xHi, 0, xHi, height());

    // Manual-notch / SAF marker over the passband tint, full height. Orange =
    // notch (rejecting that band), green = SAF (peaking it — same engine).
    if (notchOn_) {
        const QColor fill = notchPeak_ ? QColor(60, 200, 110, 70)
                                       : QColor(255, 140, 0, 70);
        const QColor line = notchPeak_ ? QColor(80, 225, 130)
                                       : QColor(255, 160, 40);
        const int xnLo = hzToX(notchRfHz_ - notchWidthHz_ / 2);
        const int xnHi = hzToX(notchRfHz_ + notchWidthHz_ / 2);
        p.fillRect(QRect(QPoint(xnLo, 0), QPoint(xnHi, height())), fill);
        p.setPen(QPen(line, 1));
        const int xnC = hzToX(notchRfHz_);
        p.drawLine(xnC, 0, xnC, height());
        p.drawText(xnC + 3, hSpec - 6, notchPeak_ ? "SAF" : "N");
    }

    // Frequency scale band on the divider (after the tints, so it stays clean),
    // then the VFO markers crossing everything. Colors follow the routing
    // panel: red = the transmitting VFO, green = the receiving one. Normal
    // (TX+RX both on A) keeps the plain center marker and shows B, when it's
    // in view, as a quiet blue sub-RX line.
    drawPrivileges(p, hSpec);
    drawScaleBand(p, hSpec);
    {
        const QFont savedFont = p.font();
        auto flag = [&](int x, const QString& text, const QColor& c) {
            QFont ff = savedFont;
            ff.setPixelSize(9);
            ff.setBold(true);
            p.setFont(ff);
            QRect r(x + 4, 18, p.fontMetrics().horizontalAdvance(text) + 8, 13);
            if (r.right() > width() - 2) r.moveRight(x - 4);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(c.red(), c.green(), c.blue(), 200));
            p.drawRoundedRect(r, 2, 2);
            p.setPen(QColor(250, 252, 255));
            p.drawText(r, Qt::AlignCenter, text);
        };
        // Role colors match the routing buttons: red = transmitting VFO,
        // green = the main receiver's VFO, blue = a parked sub dial.
        const QColor txRed(224, 93, 93), rxGreen(62, 207, 122), subBlue(93, 178, 240);
        QColor aCol(200, 80, 80);
        QString aText = "A";
        if (vfoBRole_ == 'T')      { aCol = rxGreen; aText = "A RX"; }
        else if (vfoBRole_ == 'R') { aCol = txRed;   aText = "A TX"; }
        p.setPen(aCol);
        const int xa = hzToX(0);        // A dial: center classic, floats in CTUN
        p.drawLine(xa, 0, xa, height());
        flag(xa, aText, aCol);
        if (vfoBVisible_ && vfoBHz_ && centerHz_) { // VFO B, if inside the view
            const qint64 off = static_cast<qint64>(vfoBHz_) - static_cast<qint64>(centerHz_);
            const int xb = hzToX(static_cast<int>(off));
            if (std::llabs(off - viewShiftHz_) < viewSpanHz_ / 2
                && xb >= 0 && xb <= width()) {
                // Full VFO look, same as the main passband: filter tint across
                // spectrum + waterfall, edge lines, dial line, flag.
                const QColor& c = vfoBRole_ == 'T' ? txRed
                                : vfoBRole_ == 'R' ? rxGreen : subBlue;
                const QString bText = vfoBRole_ == 'T' ? "B TX"
                                    : vfoBRole_ == 'R' ? "B RX" : "B";
                const int xLo = hzToX(static_cast<int>(off) + vfoBLo_);
                const int xHi = hzToX(static_cast<int>(off) + vfoBHi_);
                p.setBrush(Qt::NoBrush);
                p.fillRect(QRect(QPoint(xLo, 0), QPoint(xHi, height())),
                           QColor(c.red(), c.green(), c.blue(), 42));
                p.setPen(QPen(QColor(c.red(), c.green(), c.blue(), 170), 1));
                p.drawLine(xLo, 0, xLo, height());
                p.drawLine(xHi, 0, xHi, height());
                p.setPen(QPen(c, 1));
                p.drawLine(xb, 0, xb, height());
                flag(xb, bText, c);
            }
        }
        p.setFont(savedFont);
        // The flags set a fill brush; clear it or the trace path below is
        // rendered as a giant filled polygon in the flag's color.
        p.setBrush(Qt::NoBrush);
    }

    // Peak-hold trace (under the live trace), then the spectrum trace itself.
    if (n >= 2) {
        if (ds_.peakHold && peaks_.size() == spectrum_.size()) {
            p.setPen(QColor(235, 235, 245, 150));
            const int nv = binHi - binLo;
            QPointF prev;
            for (int i = 0; i < nv; ++i) {
                const double x = static_cast<double>(i) / (nv - 1) * width();
                const float t = (peaks_[binLo + i] - dbMin_) / range;
                const double y = hSpec * (1.0 - std::clamp(t, 0.0f, 1.0f));
                QPointF pt(x, y);
                if (i) p.drawLine(prev, pt);
                prev = pt;
            }
        }
        static const QColor kTraceColors[] = {
            QColor(210, 245, 215),                 // soft (the original)
            QColor(242, 244, 248),                 // white
            QColor(80, 230, 120),                  // green
            QColor(255, 216, 50),                  // yellow
            QColor(96, 200, 255),                  // cyan
        };
        p.setPen(kTraceColors[std::clamp(ds_.traceColor, 0, 4)]);
        p.drawPath(tracePath);
        drawMarkers(p, hSpec);                     // pinned frequencies
        drawSpots(p, hSpec);                       // cluster spots, on top
        drawCursor(p, hSpec);                      // hover line + zap preview
        drawSolarPanel(p, hSpec);                  // space-weather corner box
        drawCompassRose(p, hSpec);                 // bearing rose, bottom-left
    } else {
        p.setPen(QColor(120, 220, 140));
        p.drawText(QRect(0, 0, width(), hSpec), Qt::AlignCenter,
                   "panadapter — no IQ source (build with -DBUILD_SDRPLAY=ON)");
    }

    // Span readout (the scale band draws its own separators).
    p.setPen(QColor(200, 200, 200, 160));
    p.drawText(6, 14, QString("span %1 kHz   wheel: tune (shift fine)  drag: tune  "
                              "edge: bw  shift+edge: cut  ctrl+body: pbt  rclick: VFO B")
                          .arg(viewSpanHz_ / 1000.0, 0, 'f', viewSpanHz_ < 20000 ? 1 : 0));
}

void PanadapterWidget::wheelEvent(QWheelEvent* e) {
    const double steps = e->angleDelta().y() / 120.0;
    if (steps == 0.0) return;
    // NOTE: no wheel gesture on the dB axis — a plain wheel there used to
    // adjust RANGE, which silently rescaled the colors whenever a wheel-tune
    // happened near the left edge. Range lives in the DISPLAY panel; the axis
    // drag still shifts REF.
    if (e->modifiers() & Qt::ControlModifier) {     // Ctrl+wheel = zoom
        const double factor = std::pow(1.25, -steps);
        const int newSpan =
            std::clamp(static_cast<int>(std::lround(viewSpanHz_ * factor)),
                       kMinViewSpanHz, maxViewSpanHz());
        // PowerSDR CTUN rule: zoom IN centers the view on the VFO first.
        if (newSpan < viewSpanHz_ && viewShiftHz_ != 0) viewShiftHz_ = 0;
        viewSpanHz_ = newSpan;
        clampViewShift();
        wfDirty_ = true;
        emit viewSpanChanged(viewSpanHz_);
        update();
    } else {
        const int n = static_cast<int>(steps > 0 ? std::ceil(steps) : std::floor(steps));
        // Wheel over the notch marker widens/narrows it; elsewhere it tunes
        // (Shift = fine).
        if (overNotch(static_cast<int>(e->position().x())))
            emit notchWidthAdjustRequested(n);
        else if (wheelVfo_ == 'B')                  // wheel follows last-tuned VFO
            emit vfoBStepRequested(n, e->modifiers() & Qt::ShiftModifier);
        else
            emit tuneStepRequested(n, e->modifiers() & Qt::ShiftModifier);
    }
    e->accept();
}

void PanadapterWidget::leaveEvent(QEvent*) {
    hoverX_ = -1;
    update();
}

void PanadapterWidget::mousePressEvent(QMouseEvent* e) {
    const int x = e->pos().x();
    const int y = e->pos().y();
    const int edgeTol = 6;
    dragStartX_  = x;
    dragStartY_  = y;
    dragStartLo_ = pbLoHz_;
    dragStartHi_ = pbHiHz_;
    // Right-click on a spot callsign = QRZ lookup in the browser (KE9NS);
    // anywhere else = drop VFO B on that frequency (get the TX dead on the
    // station the DX just worked, one click).
    if (e->button() == Qt::RightButton) {
        drag_ = Drag::None;
        for (const SpotHit& h : spotHits_)
            if (h.rect.contains(e->pos())) {
                QDesktopServices::openUrl(
                    QUrl("https://www.qrz.com/db/" + h.call));
                return;
            }
        if (overRose(x, y)) {                      // right-click rose = clear
            roseBearing_ = -1.0;
            roseLabel_.clear();
            emit roseBearingChanged(-1.0, QString());
            update();
            return;
        }
        if (!inScaleBand(y) && !inDbAxis(x, y)) {
            // Shift+right-click = pin/unpin a frequency marker here; plain
            // right-click keeps its VFO B meaning.
            if (e->modifiers() & Qt::ShiftModifier) {
                if (centerHz_)
                    emit markerToggleRequested(static_cast<qint64>(centerHz_)
                                               + xToHz(x));
                return;
            }
            wheelVfo_ = 'B';                        // wheel now nudges B
            emit vfoBTuneRequested(xToHz(x));
        }
        return;
    }
    // The frequency-scale band IS the split handle (KE9NS-style).
    if (inScaleBand(y)) {
        drag_ = Drag::Divider;
        dragStartSplit_ = ds_.split;
        return;
    }
    // The notch is the narrowest target — give it priority over the passband
    // edges it may sit on top of.
    if (overNotch(x)) {
        drag_ = Drag::Notch;
        return;
    }
    // Click inside the compass rose = manual heading (right-click clears).
    if (overRose(x, y)) {
        drag_ = Drag::None;
        const int hSpec = spectrumHeight();
        const int cx = 10 + kRoseR, cy = hSpec - kRoseR - 10;
        roseBearing_ = std::atan2(double(x - cx), double(cy - y)) * 180.0 / M_PI;
        if (roseBearing_ < 0.0) roseBearing_ += 360.0;
        roseDistKm_ = -1.0;
        roseLabel_ = "MAN";
        emit roseBearingChanged(roseBearing_, roseLabel_);
        update();
        return;
    }
    // A click on a spot callsign tunes to the spotted frequency — and swings
    // the rose to the station when its location is known (POTA park coords
    // or cty.dat country placement).
    for (const SpotHit& h : spotHits_) {
        if (h.rect.contains(e->pos())) {
            drag_ = Drag::None;
            wheelVfo_ = 'A';
            if (h.lat < 500.0) pointRoseAt(h.lat, h.lon, h.call);
            // Ctrl+click = tune AND turn the antenna there (optional per
            // click, never automatic; bearing from cty/POTA placement).
            if (e->modifiers() & Qt::ControlModifier)
                emit spotTurnRequested(
                    h.lat < 500.0 ? roseBearing_ : -1.0, h.call);
            emit spotClicked(h.call, h.kind, h.tag);
            emit tuneRequested(static_cast<int>(h.hz - static_cast<qint64>(centerHz_)));
            return;
        }
    }
    // Left dB scale: vertical drag shifts the reference level (grid drag in
    // PowerSDR). Checked after the notch but before the passband edges.
    if (inWfDbAxis(x, y)) {
        drag_ = Drag::WfDbAxis;
        dragStartRef_ = ds_.wfRefDb;
        return;
    }
    if (inDbAxis(x, y)) {
        drag_ = Drag::DbAxis;
        dragStartRef_ = ds_.refDb;
        return;
    }
    bool onLo = std::abs(x - hzToX(pbLoHz_)) <= edgeTol;
    bool onHi = std::abs(x - hzToX(pbHiHz_)) <= edgeTol;
    if (onLo && onHi) {                             // narrow filter: closer edge wins
        if (std::abs(x - hzToX(pbLoHz_)) <= std::abs(x - hzToX(pbHiHz_))) onHi = false;
        else                                                              onLo = false;
    }
    if (onLo || onHi) {
        // Plain edge = pure bandwidth (PBT untouched, matches the front-panel
        // BW knob): the anchored zero-beat edge stays pinned, so in SSB only
        // the outer edge is a width handle — a plain grab of the pinned edge
        // adjusts that cut instead. Shift+edge = hi/lo-cut explicitly.
        if (e->modifiers() & Qt::ShiftModifier) drag_ = onLo ? Drag::LoEdge : Drag::HiEdge;
        else if (bwAnchor_ < 0 && onLo)         drag_ = Drag::LoEdge;
        else if (bwAnchor_ > 0 && onHi)         drag_ = Drag::HiEdge;
        else                                    drag_ = Drag::SymEdge;
        emit passbandEditBegan(pbLoHz_, pbHiHz_);   // consumer anchors radio state
        return;
    }
    // VFO B filter edges: same gesture language as A's (plain outer edge =
    // width, plain zero-beat edge = cut, Shift = explicit cut) driving the
    // sub RX filter. Checked before the B body grab so the edges win.
    if (vfoBVisible_ && vfoBHz_ && centerHz_) {
        const int bOff = static_cast<int>(static_cast<qint64>(vfoBHz_)
                                          - static_cast<qint64>(centerHz_));
        bool onBLo = std::abs(x - hzToX(bOff + vfoBLo_)) <= edgeTol;
        bool onBHi = std::abs(x - hzToX(bOff + vfoBHi_)) <= edgeTol;
        if (onBLo && onBHi) {                       // narrow filter: closer wins
            if (std::abs(x - hzToX(bOff + vfoBLo_))
                <= std::abs(x - hzToX(bOff + vfoBHi_))) onBHi = false;
            else                                        onBLo = false;
        }
        if (onBLo || onBHi) {
            if (e->modifiers() & Qt::ShiftModifier)
                drag_ = onBLo ? Drag::BLoEdge : Drag::BHiEdge;
            else if (bwAnchor_ < 0 && onBLo) drag_ = Drag::BLoEdge;
            else if (bwAnchor_ > 0 && onBHi) drag_ = Drag::BHiEdge;
            else                             drag_ = Drag::BSymEdge;
            wheelVfo_ = 'B';
            dragStartBLo_ = vfoBLo_;
            dragStartBHi_ = vfoBHi_;
            emit vfoBEditBegan(vfoBLo_, vfoBHi_);   // consumer anchors sub state
            return;
        }
    }
    // Grab VFO B (line or its passband tint) to slide it — split TX placement.
    if (overVfoB(x)) {
        if (lockB_) return;                         // locked: not a tune target
        drag_ = Drag::VfoB;
        wheelVfo_ = 'B';
        dragStartBOff_ = static_cast<qint64>(vfoBHz_) - static_cast<qint64>(centerHz_);
        return;
    }
    // Grab the A dial line to drag-tune the main VFO.
    if (std::abs(x - hzToX(0)) <= 4 && centerHz_) {
        if (lockA_) return;
        drag_ = Drag::VfoA;
        wheelVfo_ = 'A';
        dragStartCenter_ = centerHz_;
        return;
    }
    if (x > hzToX(pbLoHz_) && x < hzToX(pbHiHz_)) {
        // Inside the passband: dragging moves the VFO itself (drag-to-tune);
        // Ctrl+drag slides the PBT instead. Released without moving it's a
        // click-to-tune. Decided on first move. A locked A still allows the
        // Ctrl+PBT slide — the lock protects the dial, not the filter.
        bodyPbt_ = e->modifiers() & Qt::ControlModifier;
        if (lockA_ && !bodyPbt_) return;
        drag_ = Drag::BodyPending;
        dragStartCenter_ = centerHz_;
        return;
    }
    drag_ = Drag::None;
    wheelVfo_ = 'A';
    if (lockA_) return;                             // locked: no click-to-tune
    emit tuneRequested(xToHz(x),                    // click-to-tune
                       e->modifiers().testFlag(Qt::ShiftModifier));
}

void PanadapterWidget::mouseMoveEvent(QMouseEvent* e) {
    if (drag_ == Drag::None) {
        hoverX_ = e->pos().x();            // cursor line follows the mouse
        // Hover feedback: vertical-drag targets, then every horizontal grab —
        // VFO B (line/passband), the A dial line, and BOTH of A's filter
        // edges (the outer edge used to miss the double arrow).
        const int hx = e->pos().x(), hy = e->pos().y();
        const bool onAEdge = std::abs(hx - hzToX(pbLoHz_)) <= 6
                          || std::abs(hx - hzToX(pbHiHz_)) <= 6;
        bool onSpot = false;                     // spot labels are clickable
        for (const SpotHit& h : spotHits_)
            if (h.rect.contains(e->pos())) { onSpot = true; break; }
        if (inScaleBand(hy) || inDbAxis(hx, hy) || inWfDbAxis(hx, hy))
            setCursor(Qt::SizeVerCursor);
        else if (onSpot)                         setCursor(Qt::PointingHandCursor);
        else if (overVfoB(hx) || onAEdge
                 || std::abs(hx - hzToX(0)) <= 4) setCursor(Qt::SizeHorCursor);
        else                                     unsetCursor();
        return;
    }
    const int x = e->pos().x();
    const int hz = xToHz(x);
    if (drag_ == Drag::Divider) {
        // Move the spectrum/waterfall split; the waterfall re-renders from raw
        // history at the new height, so nothing smears.
        const int usable = std::max(1, height() - kScaleBandH);
        ds_.split = std::clamp(
            dragStartSplit_ + static_cast<float>(e->pos().y() - dragStartY_) / usable,
            0.15f, 0.85f);
        update();
        emit displaySettingsEdited(ds_);
        return;
    }
    if (drag_ == Drag::WfDbAxis) {
        // Same feel as the spectrum's axis, scaled to the waterfall's own
        // height; only the waterfall recolors — the trace stays put.
        const int wfTop = spectrumHeight() + kScaleBandH;
        const int wfH = std::max(1, height() - wfTop);
        const float dDb = static_cast<float>(e->pos().y() - dragStartY_)
                          * ds_.rangeDb / wfH;
        ds_.wfRefDb = std::clamp(dragStartRef_ + dDb, -100.0f, -20.0f);
        wfDbMax_ = ds_.wfRefDb;
        wfDbMin_ = ds_.wfRefDb - ds_.rangeDb;
        wfDirty_ = true;
        update();
        emit displaySettingsEdited(ds_);
        return;
    }
    if (drag_ == Drag::DbAxis) {
        // Drag the scale down = numbers slide down = ref level rises.
        const int hSpec = std::max(1, spectrumHeight());
        const float dDb = static_cast<float>(e->pos().y() - dragStartY_)
                          * ds_.rangeDb / hSpec;
        ds_.refDb = std::clamp(dragStartRef_ + dDb, -100.0f, -20.0f);
        dbMax_ = ds_.refDb;
        dbMin_ = ds_.refDb - ds_.rangeDb;
        wfDirty_ = true;
        update();
        emit displaySettingsEdited(ds_);
        return;
    }
    if (drag_ == Drag::Notch) {
        notchRfHz_ = hz;
        update();
        emit notchDragged(notchRfHz_);              // drag-to-notch
        return;
    }
    if (drag_ == Drag::VfoB) {
        const qint64 off = dragStartBOff_ + (hz - xToHz(dragStartX_));
        vfoBHz_ = static_cast<uint64_t>(static_cast<qint64>(centerHz_) + off);
        update();
        emit vfoBDragged(static_cast<int>(off));    // consumer streams *BF
        return;
    }
    if (drag_ == Drag::BSymEdge || drag_ == Drag::BLoEdge || drag_ == Drag::BHiEdge) {
        // B's filter edges, in offsets from B's own dial — same math as A's
        // edges including the SSB zero-beat anchor.
        const int bOff = static_cast<int>(static_cast<qint64>(vfoBHz_)
                                          - static_cast<qint64>(centerHz_));
        const int hzB = hz - bOff;
        if (drag_ == Drag::BSymEdge) {
            if (bwAnchor_ < 0) {
                vfoBLo_ = dragStartBLo_;
                vfoBHi_ = std::max(hzB, dragStartBLo_ + 50);
            } else if (bwAnchor_ > 0) {
                vfoBHi_ = dragStartBHi_;
                vfoBLo_ = std::min(hzB, dragStartBHi_ - 50);
            } else {
                const int center = (dragStartBLo_ + dragStartBHi_) / 2;
                const int half = std::max(25, std::abs(hzB - center));
                vfoBLo_ = center - half;
                vfoBHi_ = center + half;
            }
        } else if (drag_ == Drag::BLoEdge) {
            vfoBLo_ = std::min(hzB, vfoBHi_ - 50);
        } else {
            vfoBHi_ = std::max(hzB, vfoBLo_ + 50);
        }
        update();
        emit vfoBPassbandChanged(vfoBLo_, vfoBHi_);
        return;
    }
    if (drag_ == Drag::VfoA) {
        // Anchored to the grab-time dial: cursor movement maps 1:1 to Hz, so
        // the view recentering underneath doesn't compound.
        const qint64 delta = hz - xToHz(dragStartX_);
        emit vfoADragged(static_cast<uint64_t>(
            static_cast<qint64>(dragStartCenter_) + delta));
        return;
    }
    if (drag_ == Drag::BodyPending) {
        if (std::abs(x - dragStartX_) < 4) return;  // still just a click
        if (!bodyPbt_) {
            drag_ = Drag::VfoA;                     // drag the VFO, not the filter
            wheelVfo_ = 'A';
            const qint64 delta = hz - xToHz(dragStartX_);
            emit vfoADragged(static_cast<uint64_t>(
                static_cast<qint64>(dragStartCenter_) + delta));
            return;                                 // MUST NOT fall into edge math
        }
        drag_ = Drag::Body;                         // Ctrl: classic PBT slide
        emit passbandEditBegan(dragStartLo_, dragStartHi_);
    }
    if (drag_ == Drag::Body) {
        // Slide the whole passband: width constant -> the consumer's delta
        // decomposition sends pure PBT.
        const int delta = hz - xToHz(dragStartX_);
        pbLoHz_ = dragStartLo_ + delta;
        pbHiHz_ = dragStartHi_ + delta;
    } else if (drag_ == Drag::SymEdge) {
        // Pure bandwidth, PBT untouched (front-panel BW knob). The preview
        // must show what the radio actually does with the width: SSB grows
        // away from the carrier (zero-beat edge pinned), CW/AM/FM widen
        // symmetrically about the grab-time center.
        if (bwAnchor_ < 0) {                        // USB: low edge is zero-beat
            pbLoHz_ = dragStartLo_;
            pbHiHz_ = std::max(hz, dragStartLo_ + 50);
        } else if (bwAnchor_ > 0) {                 // LSB: high edge is zero-beat
            pbHiHz_ = dragStartHi_;
            pbLoHz_ = std::min(hz, dragStartHi_ - 50);
        } else {
            const int center = (dragStartLo_ + dragStartHi_) / 2;
            const int half = std::max(25, std::abs(hz - center));
            pbLoHz_ = center - half;
            pbHiHz_ = center + half;
        }
    } else if (drag_ == Drag::LoEdge) {
        pbLoHz_ = std::min(hz, pbHiHz_ - 50);
    } else {
        pbHiHz_ = std::max(hz, pbLoHz_ + 50);
    }
    update();
    emit passbandChanged(pbLoHz_, pbHiHz_);         // drag-to-filter
}

void PanadapterWidget::mouseDoubleClickEvent(QMouseEvent* e) {
    // Double-click either filter edge: snap that VFO's PBT back to center
    // (bandwidth kept). The first click only armed an edge drag, so nothing
    // else fired. A's edges win when the passbands overlap.
    const int x = e->pos().x();
    const int edgeTol = 6;
    if (overNotch(x)) return;
    if (std::abs(x - hzToX(pbLoHz_)) <= edgeTol
        || std::abs(x - hzToX(pbHiHz_)) <= edgeTol) {
        drag_ = Drag::None;                        // cancel the armed drag
        emit pbtZeroRequested();
        return;
    }
    if (vfoBVisible_ && vfoBHz_ && centerHz_) {
        const int bOff = static_cast<int>(static_cast<qint64>(vfoBHz_)
                                          - static_cast<qint64>(centerHz_));
        if (std::abs(x - hzToX(bOff + vfoBLo_)) <= edgeTol
            || std::abs(x - hzToX(bOff + vfoBHi_)) <= edgeTol) {
            drag_ = Drag::None;
            emit vfoBPbtZeroRequested();
        }
    }
}

void PanadapterWidget::mouseReleaseEvent(QMouseEvent* e) {
    // A press inside the passband that never moved is a click-to-tune.
    if (drag_ == Drag::BodyPending) {
        wheelVfo_ = 'A';
        emit tuneRequested(xToHz(e->pos().x()),
                           e->modifiers().testFlag(Qt::ShiftModifier));
    }
    drag_ = Drag::None;
}

} // namespace ttc
