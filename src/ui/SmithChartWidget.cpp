// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/SmithChartWidget.h"

#include <QDateTime>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <complex>

namespace ttc {

namespace {
constexpr double kZ0 = 50.0;

// Grid values, normalized to Z0. r circles cross the axis at 10/25/50/
// 100/250 ohms; the same set serves the ±x arcs.
constexpr double kGrid[] = {0.2, 0.5, 1.0, 2.0, 5.0};

QColor gridCol()  { return {0x2a, 0x36, 0x44}; }
QColor textCol()  { return {0xc8, 0xd4, 0xe0}; }
QColor faintCol() { return {0x8a, 0x98, 0xa8}; }
} // namespace

SmithChartWidget::SmithChartWidget(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setMinimumSize(340, 380);
}

void SmithChartWidget::setRuns(const QVector<PanadapterWidget::SwrRun>& runs,
                               const QString& bandLabel) {
    band_ = bandLabel;
    pts_.clear(); prevPts_.clear();
    ts_ = 0;
    // Newest run first (the sweep stores them that way); keep the first
    // two that actually carry impedance.
    for (const auto& run : runs) {
        QVector<Pt> v;
        for (const auto& p : run.pts)
            if (p.zValid)
                v.append({double(p.hz), p.rOhm, std::fabs(p.xOhm), p.swr});
        if (v.size() < 2) continue;
        if (pts_.isEmpty())        { pts_ = v; ts_ = run.ts; }
        else if (prevPts_.isEmpty()) { prevPts_ = v; break; }
    }
    auto infer = [](const QVector<Pt>& v) {
        QVector<double> a, r;
        a.reserve(v.size()); r.reserve(v.size());
        for (const auto& p : v) { a.append(p.xAbs); r.append(p.rOhm); }
        return inferXSigns(a, r);
    };
    signs_     = infer(pts_);
    prevSigns_ = infer(prevPts_);
    hover_ = -1;
    update();
}

// Deep local minima of |X| are sign crossings; the sign alternates
// between them. Same idea as the vendor's Plot ("determine sign of
// reactance/phase based on impedance and phase slopes"). A dip only
// counts when it falls well below both sides — flat-Z antennas produce
// shallow wiggles that must not flip anything (the vendor admits the
// same failure mode; that is what click-to-flip is for).
//
// The overall ORIENTATION (which segment is + and which −) cannot come
// from |X| alone, and guessing "capacitive first" is wrong whenever the
// sweep opens above a series resonance — the operator's real 80 m run
// does exactly that: X hits zero at an R peak of ~196 Ω, a textbook
// anti-resonance, X falling + -> 0 -> −, while his 40 m run crosses at
// R≈40, a series resonance rising − -> 0 -> +. Physics supplies the
// discriminator: seen through a feedline, the locus must rotate
// CLOCKWISE with rising frequency. Flipping every sign mirrors the locus
// across the real axis and exactly negates its net rotation, so compute
// the net turn once and flip wholesale if it comes out counterclockwise.
// (Both of the operator's runs validate: 40 m stays capacitive-first,
// 80 m comes out inductive-first, matching the R evidence on each.)
QVector<int> SmithChartWidget::inferXSigns(const QVector<double>& absX,
                                           const QVector<double>& rOhm) {
    const int n = int(absX.size());
    QVector<int> out(n, +1);
    if (n < 5 || rOhm.size() != n) return out;
    // If the source ever starts delivering signed X (future firmware),
    // believe it and leave the data alone.
    for (double v : absX) if (v < 0.0) return out;
    QVector<int> cross;
    for (int i = 1; i < n - 1; ++i) {
        if (absX[i] > absX[i - 1] || absX[i] > absX[i + 1]) continue;
        double lMax = 0, rMax = 0;
        for (int j = std::max(0, i - 8); j < i; ++j)
            lMax = std::max(lMax, absX[j]);
        for (int j = i + 1; j < std::min(n, i + 9); ++j)
            rMax = std::max(rMax, absX[j]);
        const double side = std::min(lMax, rMax);
        if (absX[i] < 0.4 * side && side - absX[i] > 3.0)
            if (cross.isEmpty() || i - cross.last() > 2)
                cross.append(i);
    }
    int sign = -1, k = 0;                 // candidate: capacitive first
    for (int i = 0; i < n; ++i) {
        if (k < cross.size() && i == cross[k]) { sign = -sign; ++k; }
        out[i] = sign;
    }
    double net = 0;                       // net rotation about the center
    std::complex<double> prev;
    for (int i = 0; i < n; ++i) {
        const std::complex<double> z(rOhm[i] / kZ0, out[i] * absX[i] / kZ0);
        const std::complex<double> g = (z - 1.0) / (z + 1.0);
        if (i > 0 && std::abs(g) > 1e-9 && std::abs(prev) > 1e-9)
            net += std::arg(g / prev);    // wrapped per-step turn
        prev = g;
    }
    if (net > 0)                          // counterclockwise: mirrored
        for (int& s : out) s = -s;
    return out;
}

QPointF SmithChartWidget::gamma(const Pt& p, int sign, QPointF c,
                                double R) const {
    const std::complex<double> z(p.rOhm / kZ0, sign * p.xAbs / kZ0);
    const std::complex<double> g = (z - 1.0) / (z + 1.0);
    return c + QPointF(g.real() * R, -g.imag() * R);
}

int SmithChartWidget::nearestPoint(const QPointF& pos) const {
    int best = -1;
    double bestD = 18.0 * 18.0;           // 18 px pick radius
    for (int i = 0; i < xy_.size(); ++i) {
        const QPointF d = xy_[i] - pos;
        const double dd = QPointF::dotProduct(d, d);
        if (dd < bestD) { bestD = dd; best = i; }
    }
    return best;
}

void SmithChartWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(0x14, 0x1b, 0x24));
    const double margin = 48;
    const double R = std::min(width(), height()) / 2.0 - margin;
    if (R < 80) return;
    const QPointF c(width() / 2.0, height() / 2.0);
    p.setRenderHint(QPainter::Antialiasing, true);

    QFont f = p.font();
    f.setPixelSize(11);
    p.setFont(f);

    // --- grid, clipped to the unit circle -------------------------------
    p.save();
    QPainterPath clip;
    clip.addEllipse(c, R, R);
    p.setClipPath(clip);
    for (double r : kGrid) {                       // constant-R circles
        p.setPen(QPen(gridCol(), r == 1.0 ? 1.6 : 1.0));
        const double cr = R / (1.0 + r);
        p.drawEllipse(c + QPointF(R * r / (1.0 + r), 0), cr, cr);
    }
    for (double x : kGrid) {                       // constant-±X arcs
        p.setPen(QPen(gridCol(), 1.0));
        const double cr = R / x;
        p.drawEllipse(c + QPointF(R, -cr), cr, cr);
        p.drawEllipse(c + QPointF(R, +cr), cr, cr);
    }
    for (double s : {1.5, 2.0, 3.0}) {             // SWR reference circles
        p.setPen(QPen(QColor(255, 120, 120, 80), 1, Qt::DashLine));
        const double g = (s - 1.0) / (s + 1.0);
        p.drawEllipse(c, g * R, g * R);
    }
    p.restore();
    p.setPen(QPen(QColor(0x3a, 0x4a, 0x5c), 2));   // rim + real axis
    p.drawEllipse(c, R, R);
    p.setPen(QPen(gridCol(), 1));
    p.drawLine(c - QPointF(R, 0), c + QPointF(R, 0));

    // R labels on the axis (in ohms), X labels on the rim.
    p.setPen(faintCol());
    for (double r : kGrid) {
        const double gx = (r - 1.0) / (r + 1.0);
        p.drawText(QRectF(c.x() + gx * R - 18, c.y() + 2, 36, 14),
                   Qt::AlignHCenter, QString::number(int(r * kZ0)));
    }
    for (double x : kGrid) {
        const std::complex<double> g =
            (std::complex<double>(0, x) - 1.0) /
            (std::complex<double>(0, x) + 1.0);
        const QPointF up(c.x() + g.real() * (R + 16),
                         c.y() - g.imag() * (R + 16));
        p.drawText(QRectF(up.x() - 24, up.y() - 7, 48, 14), Qt::AlignCenter,
                   QString("+j%1").arg(int(x * kZ0)));
        p.drawText(QRectF(up.x() - 24, 2 * c.y() - up.y() - 7, 48, 14),
                   Qt::AlignCenter, QString("-j%1").arg(int(x * kZ0)));
    }
    p.drawText(QRectF(c.x() - R - 26, c.y() - 16, 22, 14),
               Qt::AlignRight, "0");
    p.drawText(QRectF(c.x() + R + 4, c.y() - 16, 24, 14),
               Qt::AlignLeft, QString::fromUtf8("∞"));

    if (pts_.isEmpty()) {
        p.setPen(textCol());
        p.drawText(rect(), Qt::AlignCenter,
                   "No vector-meter sweep for this band yet.\n"
                   "Run an SWR sweep with the LP-100A enabled.");
        return;
    }

    // --- older run, dim backdrop ---------------------------------------
    if (prevPts_.size() >= 2) {
        QPolygonF poly;
        for (int i = 0; i < prevPts_.size(); ++i)
            poly << gamma(prevPts_[i], prevSigns_[i], c, R);
        p.setPen(QPen(QColor(0x6a, 0x76, 0x88, 110), 2));
        p.drawPolyline(poly);
    }

    // --- newest run: frequency-colored locus ---------------------------
    xy_.resize(pts_.size());
    for (int i = 0; i < pts_.size(); ++i)
        xy_[i] = gamma(pts_[i], signs_[i], c, R);
    const double f0 = pts_.first().fHz, f1 = pts_.last().fHz;
    auto hue = [&](double fz) {                    // low = blue, high = red
        const double t = (f1 > f0) ? (fz - f0) / (f1 - f0) : 0.0;
        return QColor::fromHsv(int(210 * (1.0 - t)), 200, 255, 235);
    };
    for (int i = 1; i < xy_.size(); ++i) {
        p.setPen(QPen(hue((pts_[i - 1].fHz + pts_[i].fHz) / 2), 3));
        p.drawLine(xy_[i - 1], xy_[i]);
    }
    for (int i = 0; i < xy_.size(); ++i) {
        p.setPen(Qt::NoPen);
        p.setBrush(hue(pts_[i].fHz));
        p.drawEllipse(xy_[i], 2.5, 2.5);
    }
    p.setBrush(Qt::NoBrush);

    // Sweep direction: hollow ring at the start, labels at both ends.
    p.setPen(QPen(hue(f0), 2));
    p.drawEllipse(xy_.first(), 5, 5);
    p.setPen(textCol());
    p.drawText(xy_.first() + QPointF(8, 4),
               QString::number(f0 / 1e6, 'f', 3));
    p.drawText(xy_.last() + QPointF(8, 4),
               QString::number(f1 / 1e6, 'f', 3));

    // Min-SWR marker (amber diamond, like the pan overlay).
    int iMin = 0;
    for (int i = 1; i < pts_.size(); ++i)
        if (pts_[i].swr < pts_[iMin].swr) iMin = i;
    {
        const QPointF m = xy_[iMin];
        QPolygonF dia;
        dia << m + QPointF(0, -7) << m + QPointF(6, 0)
            << m + QPointF(0, 7) << m + QPointF(-6, 0);
        p.setPen(QPen(QColor(120, 70, 0), 1));
        p.setBrush(QColor(255, 230, 120));
        p.drawPolygon(dia);
        p.setBrush(Qt::NoBrush);
        const QString lbl = QString("SWR %1").arg(pts_[iMin].swr, 0, 'f', 2);
        p.setPen(QColor(20, 12, 0, 220));         // shadow pass: stays
        p.drawText(m + QPointF(11, -7), lbl);     // legible over the locus
        p.setPen(QColor(255, 220, 120));
        p.drawText(m + QPointF(10, -8), lbl);
    }

    // Resonance: interpolated X=0 crossing of the signed locus. On the
    // chart that is where the curve crosses the real axis.
    for (int i = 1; i < pts_.size(); ++i) {
        const double xa = signs_[i - 1] * pts_[i - 1].xAbs;
        const double xb = signs_[i] * pts_[i].xAbs;
        if ((xa <= 0.0) == (xb <= 0.0) || xa == xb) continue;
        const double t = xa / (xa - xb);
        const double fRes = pts_[i - 1].fHz + t * (pts_[i].fHz - pts_[i - 1].fHz);
        const double rRes = pts_[i - 1].rOhm + t * (pts_[i].rOhm - pts_[i - 1].rOhm);
        Pt res; res.fHz = fRes; res.rOhm = rRes; res.xAbs = 0;
        const QPointF m = gamma(res, +1, c, R);
        p.setPen(QPen(QColor(0x5d, 0xbf, 0x7a), 2));
        p.drawEllipse(m, 6, 6);
        const QString lbl = QString("X=0  %1").arg(fRes / 1e6, 0, 'f', 3);
        p.setPen(QColor(0, 20, 8, 220));
        p.drawText(m + QPointF(11, 19), lbl);
        p.setPen(QColor(0x5d, 0xbf, 0x7a));
        p.drawText(m + QPointF(10, 18), lbl);
        break;
    }

    // Hover readout.
    if (hover_ >= 0 && hover_ < pts_.size()) {
        const Pt& h = pts_[hover_];
        p.setPen(QPen(Qt::white, 1.5));
        p.drawEllipse(xy_[hover_], 6, 6);
        p.setPen(textCol());
        f.setPixelSize(13); f.setBold(true); p.setFont(f);
        p.drawText(QRectF(10, 6, width() - 20, 40),
                   Qt::AlignLeft | Qt::AlignTop,
                   QString("%1 MHz   R %2 Ω   X %3%4 Ω   SWR %5")
                       .arg(h.fHz / 1e6, 0, 'f', 3)
                       .arg(h.rOhm, 0, 'f', 1)
                       .arg(signs_[hover_] < 0 ? "−" : "+")
                       .arg(h.xAbs, 0, 'f', 1)
                       .arg(h.swr, 0, 'f', 2));
        f.setPixelSize(11); f.setBold(false); p.setFont(f);
    }

    // Footnotes.
    p.setPen(faintCol());
    p.drawText(QRectF(10, height() - 20, width() - 20, 16),
               Qt::AlignLeft,
               "Z0 50 Ω · X sign inferred from the sweep — "
               "click a point to flip it");
    if (ts_ > 0)
        p.drawText(QRectF(10, height() - 20, width() - 20, 16),
                   Qt::AlignRight,
                   QString("%1 m · %2").arg(band_)
                       .arg(QDateTime::fromSecsSinceEpoch(ts_)
                                .toString("MMM d hh:mm")));
}

void SmithChartWidget::mouseMoveEvent(QMouseEvent* e) {
    const int h = nearestPoint(e->position());
    if (h != hover_) { hover_ = h; update(); }
}

void SmithChartWidget::mousePressEvent(QMouseEvent* e) {
    const int i = nearestPoint(e->position());
    if (i >= 0 && i < signs_.size()) {
        signs_[i] = -signs_[i];               // vendor parity: click = flip
        update();
    }
}

void SmithChartWidget::leaveEvent(QEvent*) {
    if (hover_ != -1) { hover_ = -1; update(); }
}

} // namespace ttc
