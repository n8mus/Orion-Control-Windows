// SPDX-License-Identifier: GPL-3.0-or-later
// The SmartMTR meter face, ported from AetherSDR (GPL-3; src/gui/
// SmartMtrWidget, SmartMtrConfig, SmartMtrStyle, SmartMtrGeometry,
// MeterSmoother, MeterExtremes — its design tokens, scales and ballistics).
// A flat recessed bar per measurement: RX the signal meter (S0..S9 in the
// lower half, +10..+60 over S9 in red), TX forward power and SWR — fed by the
// external wattmeter (LP-100A / PowerMaster, ~10 Hz) when it is live, the
// radio's own @STF poll otherwise.
#include "ui/SMeterWidget.h"

#include <QFontMetricsF>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

namespace ttc {

namespace {
// --- AetherSDR SmartMtrStyle.h: geometry in UNITS, palette ---------------
constexpr double kControlW = 250.0, kControlH = 35.0;
constexpr double kHoleW = 240.0, kHoleH = 10.0;
constexpr double kHoleMargX = (kControlW - kHoleW) / 2.0, kHoleMargY = 20.0;
constexpr double kHoleRadius = 2.0, kShadow = 3.0;
constexpr double kScaleMin = 10.0, kScaleMax = 230.0;   // hole-local band
constexpr double kIndicatorLine = 1.0;
constexpr double kMarkerSmallH = 4.0, kMarkerSmallW = 1.0;
constexpr double kMarkerLargeH = 5.0, kMarkerLargeW = 2.0;
constexpr double kMarkerSmallOpacity = 0.6;
constexpr double kLabelHeight = 14.0, kLabelHeightNormal = 11.0, kLabelGap = 1.0;
const QColor kControl(0x16, 0x16, 0x20);
const QColor kHole(0x5f, 0x89, 0xa2);
const QColor kBar(0xff, 0x54, 0x63);
const QColor kIndicator(0xff, 0xff, 0xff);
const QColor kShadowC(0, 0, 0, 100);
const QColor kMarkNormal(0x42, 0xe3, 0xff);
const QColor kMarkHigh(0xff, 0x54, 0x63);
// --- MeterSmoother: SmartMTR analog ballistics ----------------------------
constexpr double kAttackSec = 0.01818, kReleaseSec = 0.26940;
// --- MeterExtremes / SmartMtrExtremes -------------------------------------
constexpr qint64 kWindowMs = 3000;                       // "Medium"
constexpr double kSlewPerSec = 60.0 / (kScaleMax - kScaleMin);   // 60 units/s
// --- SmartMtrConfig: scales ------------------------------------------------
constexpr double kS0dBm = -127.0, kS9dBm = -73.0, kSMaxdBm = -13.0;

double sigFrac(double dBm) {                 // S9 pinned at the middle
    dBm = std::clamp(dBm, kS0dBm, kSMaxdBm);
    if (dBm <= kS9dBm) return 0.5 * (dBm - kS0dBm) / (kS9dBm - kS0dBm);
    return 0.5 + 0.5 * (dBm - kS9dBm) / (kSMaxdBm - kS9dBm);
}
double swrFrac(double swr) {                 // 1..2 gets 70 %, 2..3 the rest
    swr = std::clamp(swr, 1.0, 3.0);
    return swr <= 2.0 ? 0.70 * (swr - 1.0) : 0.70 + 0.30 * (swr - 2.0);
}

struct Mark {
    double frac;
    bool large, high, strong;
    QString label;
    double labelOffset;                      // UNITS, + right
};

std::vector<Mark> signalMarks() {
    std::vector<Mark> m;
    for (int s = 0; s <= 9; ++s)
        m.push_back({sigFrac(kS0dBm + s * 6.0), s % 2 == 1, false, s == 9,
                     s % 2 == 1 ? QString::number(s) : QString(), 0.0});
    for (int db = 10; db <= 60; db += 10)
        m.push_back({sigFrac(kS9dBm + db), db % 20 == 0, true, false,
                     db % 20 == 0 ? QStringLiteral("+%1").arg(db) : QString(), -4.0});
    return m;
}
std::vector<Mark> swrMarks() {
    return {{swrFrac(1.0), true, false, false, "1", 0.0},
            {swrFrac(1.5), true, false, false, "1.5", 0.0},
            {swrFrac(2.0), true, true, true, "2", 0.0},
            {swrFrac(2.5), false, true, false, QString(), 0.0},
            {swrFrac(3.0), true, true, false, "3", 0.0}};
}
std::vector<Mark> powerMarks(double fullW) {
    // Red from "rated" = full scale / 1.2 (AetherSDR's kPowerHeadroom).
    const double rated = fullW / 1.2;
    const double step = fullW <= 120 ? 20 : fullW <= 600 ? 100 : fullW <= 1200 ? 200 : 300;
    std::vector<Mark> m;
    for (double w = 0; w <= fullW + 0.5; w += step) {
        const QString lab = w >= 1000 ? QString::number(w / 1000.0, 'g', 2) + "k"
                                      : QString::number(int(w));
        m.push_back({w / fullW, true, w >= rated - 0.5, false, lab, 0.0});
    }
    if (std::fmod(rated, step) > 0.5)        // the rated boundary always marked
        m.push_back({rated / fullW, false, true, false, QString(), 0.0});
    return m;
}

struct Geo {                                 // SmartMtrGeometry::fit
    double k = 1, ox = 0, oy = 0;
    Geo(const QRectF& r) {
        k = std::min(r.width() / kControlW, r.height() / kControlH);
        ox = r.x() + (r.width() - kControlW * k) / 2.0;
        oy = r.y() + (r.height() - kControlH * k) / 2.0;
    }
    QRectF rect(double x, double y, double w, double h) const {
        return QRectF(ox + x * k, oy + y * k, w * k, h * k);
    }
    QPointF pt(double x, double y) const { return QPointF(ox + x * k, oy + y * k); }
    double len(double u) const { return u * k; }
};

// One SmartMTR control: hole, bar, inset shadow, ticks + labels, extremes,
// the type label inside the hole.
void drawControl(QPainter& p, const QRectF& area, double frac,
                 const std::vector<Mark>& marks, double maxF, double minF,
                 double extremesOpacity, const QString& type, const QFont& base) {
    const Geo g(area);
    const QRectF hole = g.rect(kHoleMargX, kHoleMargY, kHoleW, kHoleH);
    const double r = g.len(kHoleRadius);
    p.setPen(Qt::NoPen);
    p.setBrush(kHole);
    p.drawRoundedRect(hole, r, r);
    QPainterPath clip;
    clip.addRoundedRect(hole, r, r);
    const auto posU = [](double f) { return kScaleMin + std::clamp(f, 0.0, 1.0) * (kScaleMax - kScaleMin); };
    // The bar + its bright end line.
    p.save();
    p.setClipPath(clip);
    const double pos = posU(frac);
    p.fillRect(g.rect(kHoleMargX, kHoleMargY, pos, kHoleH), kBar);
    p.fillRect(g.rect(kHoleMargX + pos - kIndicatorLine, kHoleMargY, kIndicatorLine, kHoleH), kIndicator);
    // Inset shadow, all four rims.
    const double s = g.len(kShadow);
    QColor fade = kShadowC;
    fade.setAlpha(0);
    const auto band = [&](const QRectF& b, QPointF from, QPointF to) {
        QLinearGradient gr(from, to);
        gr.setColorAt(0.0, kShadowC);
        gr.setColorAt(1.0, fade);
        p.fillRect(b, gr);
    };
    band(QRectF(hole.left(), hole.top(), hole.width(), s), hole.topLeft(), hole.topLeft() + QPointF(0, s));
    band(QRectF(hole.left(), hole.bottom() - s, hole.width(), s), hole.bottomLeft(), hole.bottomLeft() - QPointF(0, s));
    band(QRectF(hole.left(), hole.top(), s, hole.height()), hole.topLeft(), hole.topLeft() + QPointF(s, 0));
    band(QRectF(hole.right() - s, hole.top(), s, hole.height()), hole.topRight(), hole.topRight() - QPointF(s, 0));
    // Type label (PWR / SWR) inside the hole, right end.
    if (!type.isEmpty()) {
        QFont f = base;
        f.setPixelSize(std::max(7, int(std::lround(g.len(7.5)))));
        f.setBold(true);
        p.setFont(f);
        p.setPen(QColor(255, 255, 255, 150));
        p.drawText(hole.adjusted(0, 0, -g.len(4), 0), Qt::AlignRight | Qt::AlignVCenter, type);
    }
    // Extremes: white triangles on the hole's bottom edge, apex up.
    if (extremesOpacity > 0.01) {
        p.setPen(Qt::NoPen);
        QColor c(255, 255, 255);
        c.setAlphaF(std::clamp(extremesOpacity, 0.0, 1.0));
        p.setBrush(c);
        for (double f : {maxF, minF}) {
            const double x = kHoleMargX + posU(f);
            const QPointF tri[3] = {g.pt(x - 2.0, kHoleMargY + kHoleH), g.pt(x + 2.0, kHoleMargY + kHoleH),
                                    g.pt(x, kHoleMargY + kHoleH - 6.0)};
            p.drawPolygon(tri, 3);
        }
    }
    p.restore();
    // Ticks above and below the hole, labels over the top ticks.
    QFont strong = base, normal = base;
    strong.setPixelSize(std::max(8, int(std::lround(g.len(kLabelHeight)))));
    strong.setWeight(QFont::Normal);
    normal.setPixelSize(std::max(8, int(std::lround(g.len(kLabelHeightNormal)))));
    normal.setWeight(QFont::Light);
    for (const Mark& m : marks) {
        const double u = posU(m.frac);
        const double h = m.large ? kMarkerLargeH : kMarkerSmallH;
        const double w = m.large ? kMarkerLargeW : kMarkerSmallW;
        QColor c = m.high ? kMarkHigh : kMarkNormal;
        if (!m.large) c.setAlphaF(kMarkerSmallOpacity);
        const double x = kHoleMargX + u - w / 2.0;
        p.fillRect(g.rect(x, kHoleMargY - h, w, h), c);
        p.fillRect(g.rect(x, kHoleMargY + kHoleH, w, h), c);
        if (m.label.isEmpty()) continue;
        p.setFont(m.strong ? strong : normal);
        p.setPen(m.high ? kMarkHigh : kMarkNormal);
        const QFontMetricsF fm(p.font());
        const double baseY = g.oy + g.len(kHoleMargY - kMarkerLargeH - kLabelGap);
        const double cx = g.ox + g.len(kHoleMargX + u + m.labelOffset);
        const double tw = fm.horizontalAdvance(m.label);
        double tx = cx - tw / 2.0;
        tx = std::clamp(tx, area.left(), area.right() - tw);   // keep end labels inside
        p.drawText(QPointF(tx, baseY - fm.descent() * 0.2), m.label);
    }
}
}  // namespace

void SMeterWidget::SmartBar::push(qint64 nowMs, double v) {
    target = std::clamp(v, 0.0, 1.0);
    win.emplace_back(nowMs, target);
    while (!win.empty() && nowMs - win.front().first > kWindowMs) win.erase(win.begin());
}

void SMeterWidget::SmartBar::tick(qint64, double dt) {
    const double d = target - disp;
    const double tau = d >= 0 ? kAttackSec : kReleaseSec;
    disp += d * (1.0 - std::exp(-dt / tau));
    double mx = 0.0, mn = 1.0;
    for (const auto& s : win) { mx = std::max(mx, s.second); mn = std::min(mn, s.second); }
    if (win.empty()) mx = mn = disp;
    // Out to a new extreme at once; back in at a constant, lazy slew.
    maxShown = mx >= maxShown ? mx : std::max(mx, maxShown - kSlewPerSec * dt);
    minShown = mn <= minShown ? mn : std::min(mn, minShown + kSlewPerSec * dt);
}

void SMeterWidget::setExtTx(double fwdWatts, double swr, double zOhm, double phaseDeg,
                            bool carrier, bool zValid, const QString& meterName) {
    extName_ = meterName;
    if (!carrier) return;                    // a key-up reading is no TX data
    extW_ = fwdWatts;
    extSwr_ = swr;
    extZ_ = zOhm;
    extPhase_ = phaseDeg;
    extZValid_ = zValid;
    sinceExt_.restart();
    // The meter sees our carrier ~10x a second: that, not the 2 Hz radio
    // poll, is what holds the TX face up through CW keying.
    tx_ = true;
    sinceTx_.restart();
}

void SMeterWidget::smartTick(qint64 now, double dt) {
    if (!tx_) {
        barSig_.push(now, haveReading_ ? sigFrac(displayDb() - 73.0) : 0.0);
        barSig_.tick(now, dt);
        return;
    }
    // Power scale auto-range: barefoot 120 W, up the tiers when an amp drives
    // the meter; drop back after 10 s below the lower tier.
    const double w = txWatts();
    static const double kTiers[] = {120.0, 600.0, 1200.0, 1800.0};
    double need = kTiers[3];
    for (double t : kTiers) if (w <= t * 0.98) { need = t; break; }
    if (need > pwrScaleW_) { pwrScaleW_ = need; sinceBigW_.restart(); }
    else if (need == pwrScaleW_) sinceBigW_.restart();
    else if (!sinceBigW_.isValid() || sinceBigW_.elapsed() > 10000) pwrScaleW_ = need;
    barPwr_.push(now, w / pwrScaleW_);
    barSwr_.push(now, w > 0.5 ? swrFrac(txSwr()) : 0.0);
    barPwr_.tick(now, dt);
    barSwr_.tick(now, dt);
}

void SMeterWidget::paintSmartMtr(QPainter& p) {
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), kControl);
    QFont base("DejaVu Sans");
    const auto spread = [](const SmartBar& b, double lo, double hi) {   // proximity fade
        return std::clamp((b.maxShown - b.minShown - lo) / (hi - lo), 0.0, 1.0);
    };
    QFont ro = base;
    if (!tx_) {
        drawControl(p, QRectF(0, 2, faceW(), 40), barSig_.disp, signalMarks(),
                    barSig_.maxShown, barSig_.minShown,
                    haveReading_ ? spread(barSig_, 0.03, 0.065)
                                       * std::clamp((displayDb() - 73.0 + 127.0) / 12.0, 0.0, 1.0)
                                 : 0.0,
                    QString(), base);
        // Readout: S-units big, dBm beside, reading mode + source tags.
        ro.setPixelSize(22);
        ro.setBold(true);
        p.setFont(ro);
        p.setPen(QColor(236, 242, 248));
        const QRectF row(12, 50, faceW() - 24, 38);
        p.drawText(row, Qt::AlignLeft | Qt::AlignVCenter, sUnitsText(displayDb()));
        ro.setPixelSize(13);
        ro.setBold(false);
        p.setFont(ro);
        p.setPen(kMarkNormal);
        if (haveReading_)
            p.drawText(row, Qt::AlignRight | Qt::AlignVCenter,
                       QString("%1 dBm").arg(int(std::lround(displayDb() - 73.0))));
        ro.setPixelSize(9);
        ro.setBold(true);
        p.setFont(ro);
        p.setPen(QColor(150, 170, 190));
        const QString tag = (mode_ == Avg ? "AVG " : mode_ == Peak ? "PK " : "")
                            + QString(source_ == SrcSdr ? "SDR" : "");
        p.drawText(QRectF(faceW() / 2.0 - 30, 50, 60, 38), Qt::AlignCenter, tag);
        return;
    }
    const double w = txWatts(), swr = txSwr();
    drawControl(p, QRectF(0, 0, faceW(), 38), barPwr_.disp, powerMarks(pwrScaleW_),
                barPwr_.maxShown, barPwr_.minShown, spread(barPwr_, 0.04, 0.12), "PWR", base);
    drawControl(p, QRectF(0, 38, faceW(), 38), barSwr_.disp, swrMarks(),
                barSwr_.maxShown, barSwr_.minShown, 0.0, "SWR", base);
    // Readout row: watts, SWR, and the vector meter's |Z| and phase.
    ro.setPixelSize(12);
    ro.setBold(true);
    p.setFont(ro);
    const QRectF row(10, 78, faceW() - 20, 17);
    p.setPen(QColor(236, 242, 248));
    p.drawText(row, Qt::AlignLeft | Qt::AlignVCenter,
               QString("%1 W").arg(w, 0, 'f', w < 10 ? 1 : 0));
    p.setPen(swr >= 2.0 ? kMarkHigh : kMarkNormal);
    p.drawText(QRectF(row.left() + 62, row.top(), 80, row.height()), Qt::AlignLeft | Qt::AlignVCenter,
               QString("SWR %1").arg(swr, 0, 'f', 2));
    ro.setBold(false);
    ro.setPixelSize(11);
    p.setFont(ro);
    p.setPen(QColor(170, 186, 204));
    QString right = extFresh() ? extName_ : QStringLiteral("radio");
    if (extFresh() && extZValid_)
        right = QString("%1 Ω %2%3°  ")
                    .arg(extZ_, 0, 'f', 0).arg(extPhase_ >= 0 ? "+" : "").arg(extPhase_, 0, 'f', 0)
                + right;
    p.drawText(row, Qt::AlignRight | Qt::AlignVCenter, right);
}

}  // namespace ttc
