// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/SMeterWidget.h"

#include <QActionGroup>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>
#include <QTimer>
#include <algorithm>
#include <cmath>

namespace ttc {

namespace {
// Meter span in dB relative to S9: S1 (-48) .. S9+60.
constexpr double kDbMin = -54.0;
constexpr double kDbMax = +60.0;
constexpr int    kPeakHoldMs = 1500;
constexpr double kMaxW = 120.0;          // TX scale top (100 W radio)


// Meter-face palettes. The Analog style is the Orion 565's own backlit
// meter: amber/gold face, black lettering, red needle. Edge/Cross keep the
// generic ivory face.
const QColor kAmberHi(244, 174, 40);
const QColor kAmberLo(219, 137, 18);
const QColor kIvoryHi(246, 239, 221);
const QColor kIvoryLo(231, 219, 188);
const QColor kBezel(40, 46, 56);
const QColor kInk(30, 28, 24);           // scale/needle black
const QColor kRed(196, 40, 32);          // over-S9 / over-100W zone
// Drake TR-7 homage face: lamp-lit blue with white markings (operator's
// photo, 2026-07-16 — "S UNITS ... DECIBELS" over "20 40 60 100 200W").
const QColor kTr7Hi(58, 118, 214);
const QColor kTr7Lo(10, 24, 62);
const QColor kTr7Ink(236, 243, 252);     // white scale lettering
const QColor kTr7Needle(14, 18, 28);     // dark silhouette needle
const QColor kNeedleRed(210, 32, 22);    // instant-reading needle
const QColor kNeedlePeak(20, 74, 160);   // peak needle (blue, Jon's ask)
const QColor kGhost(120, 116, 104);      // peak ghost needle (ivory faces)
} // namespace

SMeterWidget::SMeterWidget(QWidget* parent) : QWidget(parent) {
    QSettings s;
    style_ = std::clamp(s.value("display/meterStyle", int(Analog)).toInt(),
                        0, int(kStyleCount) - 1);
    mode_  = std::clamp(s.value("display/meterMode", int(Sig)).toInt(),
                        0, int(kModeCount) - 1);
    source_ = std::clamp(s.value("display/meterSource", int(SrcRadio)).toInt(),
                         int(SrcRadio), int(SrcSdr));
    applyStyle();
    // Needle ballistics: fast attack, slow decay, ~30 fps — poll data arrives
    // at only 2 Hz, this is what makes it move like a real movement.
    anim_ = new QTimer(this);
    anim_->setInterval(33);
    connect(anim_, &QTimer::timeout, this, [this] {
        const auto step = [](double& cur, double tgt, double tauUp, double tauDn) {
            const double tau = tgt > cur ? tauUp : tauDn;
            cur += (tgt - cur) * std::min(1.0, 0.033 / tau);
        };
        step(needleDb_, haveReading_ ? displayDb() : kDbMin, 0.10, 0.60);
        step(needleW_, tx_ ? fwdW_ : 0.0, 0.08, 0.35);
        step(needleRef_, tx_ ? refW_ : 0.0, 0.08, 0.35);
        if (style_ == Analog || style_ == Edge || style_ == Cross
            || style_ == Eye || style_ == Omni)
            update();
    });
    anim_->start();
}

void SMeterWidget::setWordmark(const QString& mark) {
    wordmark_ = mark;
    update();
}

// Hamlib TT565_STR_CAL_V2 (rigs/tentec/orion.h): raw ?S units -> dB rel S9,
// valid for firmware 2.x and later (Jon's Orion runs v3).
double SMeterWidget::rawToDbS9(int raw) {
    struct Pt { double raw, db; };
    static const Pt cal[] = {
        {10, -48}, {24, -42}, {38, -36}, {47, -30}, {61, -24}, {70, -18},
        {79, -12}, {84, -6},  {94, 0},   {103, 10}, {118, 20}, {134, 30},
        {147, 40}, {161, 50},
    };
    constexpr int n = static_cast<int>(std::size(cal));
    if (raw <= cal[0].raw)     return cal[0].db;
    if (raw >= cal[n - 1].raw) return cal[n - 1].db;
    for (int i = 1; i < n; ++i) {
        if (raw <= cal[i].raw) {
            const double f = (raw - cal[i - 1].raw) / (cal[i].raw - cal[i - 1].raw);
            return cal[i - 1].db + f * (cal[i].db - cal[i - 1].db);
        }
    }
    return cal[n - 1].db;
}

void SMeterWidget::setRawLevel(int raw) {
    // TX-face hang: on CW the radio answers in RX form between elements,
    // and flipping faces at keying speed made the TX meter unreadable
    // (live report). Hold the TX face until keying has really stopped.
    if (tx_ && sinceTx_.isValid() && sinceTx_.elapsed() < kTxHangMs) {
        if (source_ == SrcRadio) feedRx(rawToDbS9(raw));  // keep RX data warm
        return;
    }
    tx_ = false;                          // radio answered in receive
    if (source_ == SrcRadio) feedRx(rawToDbS9(raw));
    else update();                        // face may need to flip off the TX view
}

// SDR-source readings arrive already in dB rel S9 (MainWindow integrates the
// panadapter passband and applies gain compensation + the stored cal offset).
// During TX the SDR hears our own signal — ignored; the radio owns TX.
void SMeterWidget::setSdrLevel(double dbS9) {
    sdrSeen_ = true;
    if (source_ == SrcSdr && !tx_) feedRx(dbS9);
}

// Calibrating implies wanting the calibrated source — switching by hand was
// an extra step everyone (Jon, day one) forgets, and then the radio's pinned
// reading "takes over the meter" below RF gain ~25 exactly as before.
void SMeterWidget::useSdrSource() {
    source_ = SrcSdr;
    QSettings().setValue("display/meterSource", source_);
    applyStyle();
    update();
}

// Small "SDR" tag on the RX faces when the panadapter is the source — the
// two sources can legitimately read very differently (that's the point), so
// the face must say which one is talking.
void SMeterWidget::drawSdrTag(QPainter& p, double x, double y, const QColor& c) {
    if (source_ != SrcSdr || tx_) return;
    QFont f("DejaVu Sans");
    f.setPixelSize(8);
    f.setBold(true);
    p.setFont(f);
    p.setPen(c);
    p.drawText(QRectF(x, y, 30, 10), Qt::AlignLeft, "SDR");
}

// Common RX pipeline for both sources. The EMA is time-based because the
// feed rate differs wildly: radio polls at 2 Hz, SDR frames land at ~20 Hz.
void SMeterWidget::feedRx(double db) {
    dbS9_ = db;
    haveReading_ = true;
    double dt = 0.5;
    if (sinceFeed_.isValid())
        dt = std::min(2.0, sinceFeed_.elapsed() / 1000.0);
    sinceFeed_.restart();
    const double alpha = 1.0 - std::exp(-dt / 1.5);   // tau 1.5 s
    if (!emaInit_) { emaDb_ = dbS9_; emaInit_ = true; }
    else emaDb_ += alpha * (dbS9_ - emaDb_);
    if (dbS9_ >= peakDb_ || !sincePeak_.isValid()
        || sincePeak_.elapsed() > kPeakHoldMs) {
        peakDb_ = dbS9_;
        sincePeak_.restart();
    }
    update();
}

void SMeterWidget::setTxLevel(double fwdWatts, double refWatts, double swr) {
    tx_ = true;                           // radio answered in transmit
    sinceTx_.restart();
    fwdW_ = fwdWatts;
    refW_ = refWatts;
    swr_  = swr;
    if (fwdW_ >= peakW_ || !sinceTxPeak_.isValid()
        || sinceTxPeak_.elapsed() > kPeakHoldMs) {
        peakW_ = fwdW_;
        sinceTxPeak_.restart();
    }
    update();
}

double SMeterWidget::displayDb() const {
    return mode_ == Avg ? emaDb_ : mode_ == Peak ? peakDb_ : dbS9_;
}

// dB -> scale fraction. Real meter faces give S1..S9 the wider share of the
// arc and compress the over-S9 decades: 58 % / 42 % split here.
double SMeterWidget::scaleFrac(double db) const {
    db = std::clamp(db, kDbMin, kDbMax);
    if (db <= 0.0) return (db - kDbMin) / (0.0 - kDbMin) * 0.58;
    return 0.58 + db / kDbMax * 0.42;
}

QString SMeterWidget::sUnitsText(double db) const {
    if (!haveReading_) return "--";
    if (db >= 0.5) return QString("S9+%1").arg(static_cast<int>(std::lround(db)));
    const int s = std::clamp(static_cast<int>(std::lround(9.0 + db / 6.0)), 0, 9);
    return QString("S%1").arg(s);
}

void SMeterWidget::applyStyle() {
    switch (style_) {
        case Analog:  setFixedSize(230, 96); break;
        case Edge:    setFixedSize(280, 60); break;
        case Led:     setFixedSize(280, 42); break;
        case Cross:   setFixedSize(230, 96); break;
        case Eye:     setFixedSize(170, 96); break;
        case Omni:    setFixedSize(280, 60); break;
        case Tr7:     setFixedSize(230, 96); break;
    }
    static const char* styleName[] = {"Orion", "Edge", "LED", "Cross-needle",
                                      "Magic eye", "Omni-VII"};
    static const char* modeName[]  = {"Signal", "Average", "Peak"};
    setToolTip(QString("S-meter — style: %1, reading: %2, RX source: %3\n"
                       "click: next style (Orion / Edge / LED / Cross-needle"
                       " / Magic eye / Omni-VII)\n"
                       "right-click: reading mode, RX source (Radio / SDR),"
                       " SDR calibration\n"
                       "SDR source reads the panadapter — immune to the"
                       " radio's RF gain setting")
                   .arg(styleName[style_]).arg(modeName[mode_])
                   .arg(source_ == SrcSdr ? "SDR" : "Radio"));
}

void SMeterWidget::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        style_ = (style_ + 1) % kStyleCount;
        QSettings().setValue("display/meterStyle", style_);
        applyStyle();
        update();
    } else if (e->button() == Qt::RightButton) {
        showMenu(e->globalPosition().toPoint());
    }
}

void SMeterWidget::showMenu(const QPoint& globalPos) {
    QMenu menu(this);
    menu.addSection("Reading");
    auto* modes = new QActionGroup(&menu);
    static const char* modeName[] = {"Signal", "Average", "Peak hold"};
    for (int i = 0; i < kModeCount; ++i) {
        QAction* a = menu.addAction(modeName[i]);
        a->setCheckable(true);
        a->setChecked(mode_ == i);
        modes->addAction(a);
        connect(a, &QAction::triggered, this, [this, i] {
            mode_ = i;
            QSettings().setValue("display/meterMode", mode_);
            applyStyle();
            update();
        });
    }
    menu.addSection("RX source");
    auto* srcs = new QActionGroup(&menu);
    struct { const char* name; int src; } srcDef[] = {
        {"Radio", SrcRadio}, {"SDR (panadapter)", SrcSdr}};
    for (const auto& d : srcDef) {
        QAction* a = menu.addAction(d.name);
        a->setCheckable(true);
        a->setChecked(source_ == d.src);
        a->setEnabled(d.src == SrcRadio || sdrSeen_);
        srcs->addAction(a);
        connect(a, &QAction::triggered, this, [this, s = d.src] {
            source_ = s;
            QSettings().setValue("display/meterSource", source_);
            applyStyle();
            update();
        });
    }
    menu.addSeparator();
    QAction* cal = menu.addAction("Calibrate SDR to radio (RF gain full up first)");
    cal->setEnabled(sdrSeen_);
    connect(cal, &QAction::triggered, this, &SMeterWidget::calibrateRequested);
    menu.exec(globalPos);
}

void SMeterWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(12, 16, 22));
    switch (style_) {
        case Analog:  paintAnalog(p); break;
        case Edge:    paintEdge(p); break;
        case Led:     paintLed(p); break;
        case Cross:   paintCross(p); break;
        case Eye:     paintEye(p); break;
        case Omni:    paintOmni(p); break;
        case Tr7:     paintTr7(p); break;
    }
}

// ------------------------------------------------------------- Analog style
// The Orion 565's own meter face: amber backlit background, black lettering,
// dual printed scales — S-units along the top arc, watts in smaller print on
// the inner arc (transmit reads the same face on the lower scale, exactly
// like the real movement). Red needle = instant reading, blue needle = peak.
// "ORION" wordmark only — deliberately no TT emblem (nominative use stays
// clean; the word is also this console's own product name).

void SMeterWidget::paintAnalog(QPainter& p) {
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF face(1.5, 1.5, width() - 3.0, height() - 3.0);

    // Bezel + amber backlit face.
    p.setPen(QPen(kBezel, 3));
    QLinearGradient g(face.topLeft(), face.bottomLeft());
    g.setColorAt(0.0, kAmberHi);
    g.setColorAt(1.0, kAmberLo);
    p.setBrush(g);
    p.drawRoundedRect(face, 7, 7);

    const QPointF pivot(width() / 2.0, height() + 46.0);
    const double Rs = pivot.y() - 13.0;              // S scale arc (outer)
    const double Rw = Rs - 22.0;                     // power scale (inner)
    const double aL = 122.0, aR = 58.0;              // sweep, math degrees
    const auto ptAt = [&](double frac, double r) {
        const double a = (aL - (aL - aR) * frac) * M_PI / 180.0;
        return QPointF(pivot.x() + r * std::cos(a), pivot.y() - r * std::sin(a));
    };
    const QRectF arcRectS(pivot.x() - Rs, pivot.y() - Rs, 2 * Rs, 2 * Rs);
    const auto spanArc = [&](const QRectF& ar, double f0, double f1,
                             const QColor& c, double wid) {
        p.setPen(QPen(c, wid));
        const double a0 = aL - (aL - aR) * f0, a1 = aL - (aL - aR) * f1;
        p.drawArc(ar, static_cast<int>(a1 * 16),
                  static_cast<int>((a0 - a1) * 16));
    };

    // --- top scale: S-units, black lettering, thin red arc over S9 --------
    spanArc(arcRectS, 0.0, scaleFrac(0.0), kInk, 1.6);
    spanArc(arcRectS, scaleFrac(0.0), 1.0, kRed, 2.6);
    QFont f = p.font(); f.setPixelSize(9); f.setBold(true); p.setFont(f);
    const auto tickS = [&](double frac, bool major) {
        p.setPen(QPen(kInk, major ? 1.6 : 1.0));
        p.drawLine(ptAt(frac, Rs - (major ? 7 : 4)), ptAt(frac, Rs));
    };
    const auto labelAt = [&](double frac, double r, const QString& t) {
        const QPointF pt = ptAt(frac, r);
        p.drawText(QRectF(pt.x() - 12, pt.y() - 6, 24, 12), Qt::AlignCenter, t);
    };
    for (int s = 1; s <= 9; ++s) {
        const double fr = scaleFrac((s - 9) * 6.0);
        tickS(fr, s & 1);
        if (s & 1) {
            p.setPen(kInk);
            labelAt(fr, Rs - 15, QString::number(s));
        }
    }
    for (int over = 20; over <= 60; over += 20) {
        const double fr = scaleFrac(over);
        tickS(fr, true);
        p.setPen(kInk);
        labelAt(fr, Rs - 15, QString("+%1").arg(over));
    }

    // --- bottom scale: watts in smaller print on the inner arc -------------
    QFont fw = p.font(); fw.setPixelSize(7); fw.setBold(true); p.setFont(fw);
    for (int w = 0; w <= 120; w += 10) {
        const double fr = w / kMaxW;
        p.setPen(QPen(w > 100 ? kRed : kInk, w % 50 == 0 ? 1.4 : 0.9));
        p.drawLine(ptAt(fr, Rw - (w % 50 == 0 ? 5 : 3)), ptAt(fr, Rw));
        if (w % 50 == 0 || w == 100) {
            p.setPen(w > 100 ? kRed : kInk);
            labelAt(fr, Rw - 11, QString::number(w));
        }
    }

    // Wordmark between the scales and the pivot — names the connected radio.
    QFont fo("DejaVu Sans");
    fo.setPixelSize(10);
    fo.setBold(true);
    fo.setLetterSpacing(QFont::AbsoluteSpacing, 2.0);
    p.setFont(fo);
    p.setPen(kInk);
    p.drawText(QRectF(0, height() - 34, width(), 12), Qt::AlignHCenter,
               wordmark_);

    // --- needles: blue peak underneath, red instant on top -----------------
    // RX reads the S scale, TX reads the power scale — one movement, two
    // printed scales, like the radio itself.
    const bool rx = !tx_;
    const double instFrac = rx ? scaleFrac(needleDb_)
                               : std::clamp(needleW_ / kMaxW, 0.0, 1.0);
    const double peakFrac = rx ? scaleFrac(peakDb_)
                               : std::clamp(peakW_ / kMaxW, 0.0, 1.0);
    if (haveReading_ || !rx) {
        p.setPen(QPen(kNeedlePeak, 1.6, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(ptAt(peakFrac, Rs * 0.40), ptAt(peakFrac, Rs - 6));
    }
    p.setPen(QPen(kNeedleRed, 2.4, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(ptAt(instFrac, Rs * 0.36), ptAt(instFrac, Rs - 5));

    // Readouts on the face bottom.
    f.setPixelSize(12); p.setFont(f);
    p.setPen(kInk);
    if (rx) {
        p.drawText(QRectF(face.left() + 8, face.bottom() - 18, 90, 15),
                   Qt::AlignLeft | Qt::AlignVCenter, sUnitsText(displayDb()));
    } else {
        p.drawText(QRectF(face.left() + 8, face.bottom() - 18, 90, 15),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QString("%1 W").arg(fwdW_, 0, 'f', 0));
        p.setPen(swr_ > 2.5 ? kRed : kInk);
        p.drawText(QRectF(face.right() - 98, face.bottom() - 18, 90, 15),
                   Qt::AlignRight | Qt::AlignVCenter,
                   QString("SWR %1").arg(swr_, 0, 'f', 1));
    }
    if (rx && mode_ != Sig) {                        // reading-mode chip
        f.setPixelSize(9); p.setFont(f);
        p.setPen(kRed);
        p.drawText(QRectF(face.right() - 40, face.bottom() - 16, 32, 12),
                   Qt::AlignRight, mode_ == Avg ? "AVG" : "PK");
    }
    drawSdrTag(p, face.left() + 8, face.top() + 5, kInk);
}

// ---------------------------------------------------------------- TR-7 style
// Drake TR-7 homage: wide blue lamp-lit face, white markings, S-units +
// decibels-over on the outer arc, the TR-7's 200 W power scale on the
// inner. One movement, two printed scales, exactly like the original —
// RX reads the top, TX the bottom.
void SMeterWidget::paintTr7(QPainter& p) {
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF face(1.5, 1.5, width() - 3.0, height() - 3.0);
    p.setPen(QPen(kBezel, 3));
    QLinearGradient g(face.topLeft(), face.bottomLeft());
    g.setColorAt(0.0, kTr7Hi);
    g.setColorAt(1.0, kTr7Lo);
    p.setBrush(g);
    p.drawRoundedRect(face, 7, 7);
    {   // soft lamp glow behind the lettering
        QRadialGradient rg(QPointF(width() / 2.0, face.top() + 18),
                           width() * 0.55);
        rg.setColorAt(0.0, QColor(120, 175, 255, 70));
        rg.setColorAt(1.0, QColor(0, 0, 0, 0));
        p.setPen(Qt::NoPen);
        p.setBrush(rg);
        p.drawRoundedRect(face, 7, 7);
    }

    const QPointF pivot(width() / 2.0, height() + 46.0);
    const double Rs = pivot.y() - 13.0;
    const double Rw = Rs - 22.0;
    const double aL = 122.0, aR = 58.0;
    const auto ptAt = [&](double frac, double r) {
        const double a = (aL - (aL - aR) * frac) * M_PI / 180.0;
        return QPointF(pivot.x() + r * std::cos(a), pivot.y() - r * std::sin(a));
    };
    const QRectF arcRectS(pivot.x() - Rs, pivot.y() - Rs, 2 * Rs, 2 * Rs);
    p.setPen(QPen(kTr7Ink, 1.4));
    p.drawArc(arcRectS, static_cast<int>(aR * 16),
              static_cast<int>((aL - aR) * 16));

    QFont f = p.font(); f.setPixelSize(9); f.setBold(true); p.setFont(f);
    const auto labelAt = [&](double frac, double r, const QString& t) {
        const QPointF pt = ptAt(frac, r);
        p.drawText(QRectF(pt.x() - 13, pt.y() - 6, 26, 12), Qt::AlignCenter, t);
    };
    // top scale: S 1..9 (odd labels), then decibels-over 10/30/60
    for (int sN = 1; sN <= 9; ++sN) {
        const double fr = scaleFrac((sN - 9) * 6.0);
        p.setPen(QPen(kTr7Ink, sN & 1 ? 1.6 : 1.0));
        p.drawLine(ptAt(fr, Rs - (sN & 1 ? 7 : 4)), ptAt(fr, Rs));
        if (sN & 1) {
            p.setPen(kTr7Ink);
            labelAt(fr, Rs - 15, QString::number(sN));
        }
    }
    for (int over : {10, 30, 60}) {
        const double fr = scaleFrac(over);
        p.setPen(QPen(kTr7Ink, 1.6));
        p.drawLine(ptAt(fr, Rs - 7), ptAt(fr, Rs));
        p.setPen(kTr7Ink);
        // Label nudges (ticks stay true): +10 clears S9 to its right,
        // which then crowded 30 — 30 slides toward 60 (operator-tuned).
        const double lf = over == 10 ? fr + 0.035
                        : over == 30 ? fr + 0.030 : fr;
        labelAt(lf, Rs - 15, over == 10 ? "+10" : QString::number(over));
    }
    // captions, photo-faithful: S UNITS left, DECIBELS right
    QFont fc = p.font(); fc.setPixelSize(7); fc.setBold(true);
    fc.setLetterSpacing(QFont::AbsoluteSpacing, 1.0);
    p.setFont(fc);
    p.setPen(kTr7Ink);
    p.drawText(QRectF(face.left() + 8, face.top() + 4, 90, 10),
               Qt::AlignLeft, "S UNITS");
    p.drawText(QRectF(face.right() - 98, face.top() + 4, 90, 10),
               Qt::AlignRight, "DECIBELS");

    // bottom scale: the TR-7's 200 W line (a 100 W rig reads mid-scale,
    // exactly as it would on the real meter)
    constexpr double kTr7MaxW = 200.0;
    // Square-law watt scale (voltage-linear), like real wattmeter faces —
    // a linear map crammed 20/40/60 into the left corner (screenshot-found).
    const auto wFrac = [&](double w) {
        return std::sqrt(std::clamp(w / kTr7MaxW, 0.0, 1.0));
    };
    QFont fw = p.font(); fw.setPixelSize(7); fw.setBold(true); p.setFont(fw);
    for (int w : {20, 40, 60, 100, 200}) {
        const double fr = wFrac(w);
        p.setPen(QPen(kTr7Ink, w >= 100 ? 1.5 : 1.0));
        p.drawLine(ptAt(fr, Rw - 5), ptAt(fr, Rw));
        p.setPen(kTr7Ink);
        // 40/60 print too close at this size — spread the labels apart
        // (40 toward 20, 60 toward 100); ticks stay true.
        const double lf = w == 40 ? fr - 0.018
                        : w == 60 ? fr + 0.018 : fr;
        labelAt(lf, Rw - 11,
                w == 200 ? QStringLiteral("200W") : QString::number(w));
    }

    // homage wordmark
    QFont fo("DejaVu Sans");
    fo.setPixelSize(10);
    fo.setBold(true);
    fo.setLetterSpacing(QFont::AbsoluteSpacing, 2.0);
    p.setFont(fo);
    p.setPen(kTr7Ink);
    p.drawText(QRectF(0, height() - 34, width(), 12), Qt::AlignHCenter,
               "TR-7");

    // needles: pale peak ghost under a dark silhouette needle
    const bool rx = !tx_;
    const double instFrac = rx ? scaleFrac(needleDb_) : wFrac(needleW_);
    const double peakFrac = rx ? scaleFrac(peakDb_) : wFrac(peakW_);
    if (haveReading_ || !rx) {
        p.setPen(QPen(QColor(190, 215, 250, 150), 1.6, Qt::SolidLine,
                      Qt::RoundCap));
        p.drawLine(ptAt(peakFrac, Rs * 0.40), ptAt(peakFrac, Rs - 6));
    }
    p.setPen(QPen(kTr7Needle, 2.4, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(ptAt(instFrac, Rs * 0.36), ptAt(instFrac, Rs - 5));

    // readouts
    f.setPixelSize(12); p.setFont(f);
    p.setPen(kTr7Ink);
    if (rx) {
        p.drawText(QRectF(face.left() + 8, face.bottom() - 18, 90, 15),
                   Qt::AlignLeft | Qt::AlignVCenter, sUnitsText(displayDb()));
    } else {
        p.drawText(QRectF(face.left() + 8, face.bottom() - 18, 90, 15),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QString("%1 W").arg(fwdW_, 0, 'f', 0));
        p.setPen(swr_ > 2.5 ? QColor(255, 120, 110) : kTr7Ink);
        p.drawText(QRectF(face.right() - 98, face.bottom() - 18, 90, 15),
                   Qt::AlignRight | Qt::AlignVCenter,
                   QString("SWR %1").arg(swr_, 0, 'f', 1));
    }
    if (rx && mode_ != Sig) {
        f.setPixelSize(9); p.setFont(f);
        p.setPen(QColor(255, 170, 150));
        p.drawText(QRectF(face.right() - 40, face.bottom() - 16, 32, 12),
                   Qt::AlignRight, mode_ == Avg ? "AVG" : "PK");
    }
    drawSdrTag(p, face.left() + 8, face.top() + 15, kTr7Ink);
}

// --------------------------------------------------------------- Edge style
// Edgewise meter: linear ivory window with a vertical needle riding a
// horizontal scale.

void SMeterWidget::paintEdge(QPainter& p) {
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF face(1.5, 1.5, width() - 3.0, height() - 3.0);
    p.setPen(QPen(kBezel, 3));
    QLinearGradient g(face.topLeft(), face.bottomLeft());
    g.setColorAt(0.0, kIvoryHi);
    g.setColorAt(1.0, kIvoryLo);
    p.setBrush(g);
    p.drawRoundedRect(face, 6, 6);

    const double x0 = face.left() + 12, x1 = face.right() - 12;
    const double yScale = face.top() + 22;
    const auto fx = [&](double frac) { return x0 + (x1 - x0) * frac; };

    const bool rx = !tx_;
    const double redFrac = rx ? scaleFrac(0.0) : 100.0 / kMaxW;
    p.setPen(QPen(kInk, 1.6));
    p.drawLine(QPointF(x0, yScale), QPointF(fx(redFrac), yScale));
    p.setPen(QPen(kRed, 3.0));
    p.drawLine(QPointF(fx(redFrac), yScale), QPointF(x1, yScale));

    QFont f = p.font(); f.setPixelSize(9); f.setBold(true); p.setFont(f);
    const auto tick = [&](double frac, bool major, const QColor& c) {
        p.setPen(QPen(c, major ? 1.6 : 1.0));
        p.drawLine(QPointF(fx(frac), yScale),
                   QPointF(fx(frac), yScale + (major ? 7 : 4)));
    };
    const auto label = [&](double frac, const QString& t, const QColor& c) {
        p.setPen(c);
        p.drawText(QRectF(fx(frac) - 13, face.top() + 3, 26, 11), Qt::AlignCenter, t);
    };
    if (rx) {
        for (int s = 1; s <= 9; ++s) {
            const double fr = scaleFrac((s - 9) * 6.0);
            tick(fr, s & 1, kInk);
            if (s & 1) label(fr, QString::number(s), kInk);
        }
        for (int over = 20; over <= 60; over += 20) {
            const double fr = scaleFrac(over);
            tick(fr, true, kRed);
            label(fr, QString("+%1").arg(over), kRed);
        }
    } else {
        for (int w = 0; w <= 120; w += 10) {
            const double fr = w / kMaxW;
            const bool major = w % 25 == 0 || w == 120;
            tick(fr, major, w > 100 ? kRed : kInk);
            if (w % 50 == 0 || w == 100)
                label(fr, QString::number(w), w > 100 ? kRed : kInk);
        }
    }

    const double needleFrac = rx
        ? scaleFrac(needleDb_)
        : std::clamp(needleW_ / kMaxW, 0.0, 1.0);
    if (rx && haveReading_) {
        p.setPen(QPen(kGhost, 1.2));
        p.drawLine(QPointF(fx(scaleFrac(peakDb_)), yScale - 4),
                   QPointF(fx(scaleFrac(peakDb_)), face.bottom() - 14));
    }
    p.setPen(QPen(kInk, 2.2, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(fx(needleFrac), yScale - 5),
               QPointF(fx(needleFrac), face.bottom() - 12));

    f.setPixelSize(11); p.setFont(f);
    p.setPen(kInk);
    if (rx) {
        p.drawText(QRectF(face.left() + 8, face.bottom() - 15, 90, 13),
                   Qt::AlignLeft | Qt::AlignVCenter, sUnitsText(displayDb()));
        if (mode_ != Sig) {
            f.setPixelSize(9); p.setFont(f);
            p.setPen(kRed);
            p.drawText(QRectF(face.right() - 40, face.bottom() - 14, 32, 11),
                       Qt::AlignRight, mode_ == Avg ? "AVG" : "PK");
        }
    } else {
        p.drawText(QRectF(face.left() + 8, face.bottom() - 15, 110, 13),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QString("TX %1 W").arg(fwdW_, 0, 'f', 0));
        p.setPen(swr_ > 2.5 ? kRed : kInk);
        p.drawText(QRectF(face.right() - 98, face.bottom() - 15, 90, 13),
                   Qt::AlignRight | Qt::AlignVCenter,
                   QString("SWR %1").arg(swr_, 0, 'f', 1));
    }
    drawSdrTag(p, width() / 2.0 - 10, face.bottom() - 13, kInk);
}

// ---------------------------------------------------------------- LED style
// Segmented LED bargraph (Thetis LED item look): lit segments up to the
// reading, unlit ghosts behind, peak segment held. Green through the normal
// range, amber approaching the redline, red beyond it.

void SMeterWidget::paintLed(QPainter& p) {
    const bool rx = !tx_;
    constexpr int kSegs = 28;
    const int x0 = 6, x1 = width() - 66;
    const int segW = (x1 - x0) / kSegs;
    const int top = 14, hSeg = height() - top - 8;
    const double frac = rx
        ? scaleFrac(haveReading_ ? displayDb() : kDbMin)
        : std::clamp(fwdW_ / kMaxW, 0.0, 1.0);
    const double peakFrac = rx ? scaleFrac(peakDb_)
                               : std::clamp(peakW_ / kMaxW, 0.0, 1.0);
    const double redAt = rx ? scaleFrac(0.0) : 100.0 / kMaxW;
    const int lit = static_cast<int>(std::lround(frac * kSegs));
    const int pk  = std::clamp(static_cast<int>(std::lround(peakFrac * kSegs)) - 1,
                               0, kSegs - 1);
    for (int i = 0; i < kSegs; ++i) {
        const double f = (i + 0.5) / kSegs;
        QColor c = f < redAt * 0.82 ? QColor(62, 207, 122)      // green
                 : f < redAt        ? QColor(240, 190, 60)      // amber
                                    : QColor(235, 70, 50);      // red
        const bool on = i < lit || (haveReading_ && i == pk);
        if (!on) c.setAlpha(45);
        p.fillRect(QRect(x0 + i * segW, top, segW - 2, hSeg), c);
    }
    // Scale labels above the segments.
    QFont f = p.font(); f.setPixelSize(9); p.setFont(f);
    p.setPen(QColor(160, 170, 180));
    if (rx) {
        for (int s = 1; s <= 9; s += 2)
            p.drawText(x0 + int(scaleFrac((s - 9) * 6.0) * (x1 - x0)) - 8, 0,
                       16, 12, Qt::AlignCenter, QString::number(s));
        for (int over = 20; over <= 60; over += 20)
            p.drawText(x0 + int(scaleFrac(over) * (x1 - x0)) - 14, 0,
                       28, 12, Qt::AlignCenter, QString("+%1").arg(over));
    } else {
        for (int w = 0; w <= 100; w += 25)
            p.drawText(x0 + int(w / kMaxW * (x1 - x0)) - 10, 0,
                       20, 12, Qt::AlignCenter, QString::number(w));
    }
    // Readout to the right.
    f.setPixelSize(12); f.setBold(true); p.setFont(f);
    p.setPen(rx ? QColor(220, 230, 240) : QColor(240, 160, 40));
    p.drawText(QRect(x1 + 6, 0, width() - x1 - 8, height()),
               Qt::AlignVCenter | Qt::AlignLeft,
               rx ? sUnitsText(displayDb())
                  : QString("%1W\nSWR %2").arg(fwdW_, 0, 'f', 0)
                        .arg(swr_, 0, 'f', 1));
    drawSdrTag(p, x1 + 6, 1, QColor(96, 214, 196));
}

// --------------------------------------------------------- Cross-needle TX
// Daiwa-style crossed-needle wattmeter for transmit: FORWARD needle pivots
// bottom-left sweeping right, REFLECTED pivots bottom-right sweeping left,
// SWR read where they cross (shown as digits). On receive this style shows
// the ivory analog S-face.

void SMeterWidget::paintCross(QPainter& p) {
    if (!tx_) { paintAnalog(p); return; }
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF face(1.5, 1.5, width() - 3.0, height() - 3.0);
    p.setPen(QPen(kBezel, 3));
    QLinearGradient g(face.topLeft(), face.bottomLeft());
    g.setColorAt(0.0, kIvoryHi);
    g.setColorAt(1.0, kIvoryLo);
    p.setBrush(g);
    p.drawRoundedRect(face, 7, 7);

    const double H = height();
    const QPointF pivF(width() * 0.13, H + 30.0);   // forward: bottom-left
    const QPointF pivR(width() * 0.87, H + 30.0);   // reflected: bottom-right
    const double R = H + 30.0 - 16.0;
    // Forward sweeps 6..40 degrees right of vertical; reflected mirrors —
    // shallow enough that the two scales stay on their own halves.
    const auto fwdPt = [&](double frac, double r) {
        const double a = (6.0 + 34.0 * frac) * M_PI / 180.0;
        return QPointF(pivF.x() + r * std::sin(a), pivF.y() - r * std::cos(a));
    };
    const auto refPt = [&](double frac, double r) {
        const double a = (6.0 + 34.0 * frac) * M_PI / 180.0;
        return QPointF(pivR.x() - r * std::sin(a), pivR.y() - r * std::cos(a));
    };
    QFont f = p.font(); f.setPixelSize(8); f.setBold(true); p.setFont(f);
    // Forward scale (0..120 W): ticks every 20, labels only at the ends so
    // nothing collides where the arcs approach each other.
    for (int w = 0; w <= 120; w += 20) {
        const double fr = w / kMaxW;
        p.setPen(QPen(w > 100 ? kRed : kInk, w % 60 == 0 ? 1.5 : 1.0));
        p.drawLine(fwdPt(fr, R - 5), fwdPt(fr, R));
    }
    p.setPen(kInk);
    { const QPointF pt = fwdPt(0.0, R - 12);
      p.drawText(QRectF(pt.x() - 10, pt.y() - 5, 20, 10), Qt::AlignCenter, "0"); }
    { const QPointF pt = fwdPt(100.0 / kMaxW, R - 12);
      p.drawText(QRectF(pt.x() - 12, pt.y() - 5, 24, 10), Qt::AlignCenter, "100"); }
    // Reflected scale (0..30 W), same treatment.
    for (int w = 0; w <= 30; w += 5) {
        const double fr = w / 30.0;
        p.setPen(QPen(kInk, w % 15 == 0 ? 1.5 : 1.0));
        p.drawLine(refPt(fr, R - 5), refPt(fr, R));
    }
    { const QPointF pt = refPt(0.0, R - 12);
      p.drawText(QRectF(pt.x() - 10, pt.y() - 5, 20, 10), Qt::AlignCenter, "0"); }
    { const QPointF pt = refPt(1.0, R - 12);
      p.drawText(QRectF(pt.x() - 10, pt.y() - 5, 20, 10), Qt::AlignCenter, "30"); }
    f.setPixelSize(7); p.setFont(f);
    p.setPen(kInk);
    p.drawText(QRectF(face.left() + 7, face.top() + 4, 60, 10),
               Qt::AlignLeft, "FWD W");
    p.drawText(QRectF(face.right() - 67, face.top() + 4, 60, 10),
               Qt::AlignRight, "REF W");
    // Needles (animated): forward black, reflected red, crossing mid-face.
    p.setPen(QPen(kInk, 2.2, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(fwdPt(std::clamp(needleW_ / kMaxW, 0.0, 1.0), R * 0.30),
               fwdPt(std::clamp(needleW_ / kMaxW, 0.0, 1.0), R - 4));
    p.setPen(QPen(kRed, 1.8, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(refPt(std::clamp(needleRef_ / 30.0, 0.0, 1.0), R * 0.30),
               refPt(std::clamp(needleRef_ / 30.0, 0.0, 1.0), R - 4));
    // Readouts along the bottom, SWR big in the middle.
    f.setPixelSize(9); f.setBold(true); p.setFont(f);
    p.setPen(kInk);
    p.drawText(QRectF(face.left() + 7, face.bottom() - 15, 60, 13),
               Qt::AlignLeft, QString("%1W").arg(fwdW_, 0, 'f', 0));
    p.drawText(QRectF(face.right() - 67, face.bottom() - 15, 60, 13),
               Qt::AlignRight, QString("%1W").arg(refW_, 0, 'f', 0));
    f.setPixelSize(13); p.setFont(f);
    p.setPen(swr_ > 2.5 ? kRed : kInk);
    p.drawText(QRectF(0, face.bottom() - 18, width(), 15), Qt::AlignHCenter,
               QString("SWR %1").arg(swr_, 0, 'f', 1));
}

// ---------------------------------------------------------- Magic-eye style
// EM80-style tuning eye (Thetis MAGIC_EYE): a glowing green disc whose dark
// shadow wedge (anchored at the bottom) closes as the signal rises — full
// signal = fully open eye. Big signal beyond full scale would overlap; we
// simply saturate.

void SMeterWidget::paintEye(QPainter& p) {
    p.setRenderHint(QPainter::Antialiasing);
    const bool rx = !tx_;
    const double frac = rx
        ? scaleFrac(needleDb_)
        : std::clamp(needleW_ / kMaxW, 0.0, 1.0);
    const int R = height() / 2 - 8;
    const QPointF c(8.0 + R, height() / 2.0);
    // Tube bezel + glass background.
    p.setPen(QPen(QColor(30, 34, 40), 4));
    p.setBrush(QColor(8, 20, 10));
    p.drawEllipse(c, R + 3.0, R + 3.0);
    // Lit phosphor disc (dimmed green) with a soft radial glow.
    QRadialGradient glow(c, R);
    glow.setColorAt(0.0, QColor(140, 255, 160));
    glow.setColorAt(0.75, QColor(60, 200, 90));
    glow.setColorAt(1.0, QColor(20, 90, 40));
    p.setPen(Qt::NoPen);
    p.setBrush(glow);
    p.drawEllipse(c, double(R), double(R));
    // Shadow wedge: total dark angle = (1 - frac) * 360, centered on the
    // bottom of the disc, closing as the signal rises.
    const double darkDeg = (1.0 - std::clamp(frac, 0.0, 1.0)) * 360.0;
    if (darkDeg > 1.0) {
        QPainterPath wedge(c);
        // Qt pie angles: 0 = 3 o'clock, CCW. Bottom = 270. Span centered there.
        const double start = 270.0 - darkDeg / 2.0;
        wedge.arcTo(QRectF(c.x() - R, c.y() - R, 2.0 * R, 2.0 * R),
                    start, darkDeg);
        wedge.closeSubpath();
        p.setBrush(QColor(6, 24, 11));             // deep shadow, EM80 contrast
        p.drawPath(wedge);
    }
    // Center cap and the classic crossbar slit.
    p.setBrush(QColor(12, 26, 16));
    p.drawEllipse(c, R * 0.22, R * 0.22);
    p.setPen(QPen(QColor(10, 30, 15), 2));
    p.drawLine(QPointF(c.x(), c.y() - R), QPointF(c.x(), c.y() - R * 0.22));
    // Readout beside the tube.
    QFont f = p.font(); f.setPixelSize(12); f.setBold(true); p.setFont(f);
    p.setPen(rx ? QColor(120, 235, 150) : QColor(240, 160, 40));
    const QRect txt(int(c.x()) + R + 12, 8, width() - (int(c.x()) + R + 14),
                    height() - 16);
    if (rx) {
        p.drawText(txt, Qt::AlignTop | Qt::AlignLeft, sUnitsText(displayDb()));
        if (mode_ != Sig) {
            f.setPixelSize(9); p.setFont(f);
            p.setPen(QColor(230, 90, 70));
            p.drawText(txt, Qt::AlignBottom | Qt::AlignLeft,
                       mode_ == Avg ? "AVG" : "PK");
        }
    } else {
        p.drawText(txt, Qt::AlignTop | Qt::AlignLeft,
                   QString("%1 W").arg(fwdW_, 0, 'f', 0));
        f.setPixelSize(10); p.setFont(f);
        p.setPen(swr_ > 2.5 ? QColor(240, 70, 50) : QColor(200, 212, 224));
        p.drawText(txt, Qt::AlignBottom | Qt::AlignLeft,
                   QString("SWR %1").arg(swr_, 0, 'f', 1));
    }
    drawSdrTag(p, width() - 32.0, height() - 14.0, QColor(96, 214, 196));
}

// ------------------------------------------------------------ Omni VII style
// The Omni VII (588) draws its meter on the front-panel TFT: a white rail
// with drop ticks, a plain white bar filling beneath it, and a chunky
// readout at the lower left ("S1", "95W") — no numbers printed on the scale
// at all, the readout IS the number. Same screen palette as the radio:
// white pixels on near-black glass, cyan and yellow accents. Receive reads
// S-units; transmit reads forward power with the SWR figure in yellow (the
// real TX METER menu picks power or SWR — we just show both). Peak hold is
// a hollow LCD segment left standing on the bar.

void SMeterWidget::paintOmni(QPainter& p) {
    const bool rx = !tx_;
    const QColor kGlass(7, 11, 17);          // TFT black, slight blue cast
    const QColor kPix(226, 232, 238);        // lit "pixel" white
    const QColor kBarPix(185, 196, 208);     // bar a step dimmer than the rail
    const QColor kCyan(96, 214, 196);
    const QColor kYellow(233, 198, 62);
    const QRectF face(1.0, 1.0, width() - 2.0, height() - 2.0);
    p.fillRect(face, kGlass);
    p.setPen(QPen(QColor(50, 58, 68), 2));
    p.drawRect(face);

    const double x0 = 12, x1 = width() - 12;
    const double yRail = 17;
    const auto fx = [&](double frac) { return x0 + (x1 - x0) * frac; };
    const double frac = rx ? scaleFrac(needleDb_)
                           : std::clamp(needleW_ / kMaxW, 0.0, 1.0);
    const double peakFrac = rx ? scaleFrac(peakDb_)
                               : std::clamp(peakW_ / kMaxW, 0.0, 1.0);

    // Bar first, rail and ticks over it — one plane of pixels, like the TFT.
    if (haveReading_ || !rx) {
        p.fillRect(QRectF(x0, yRail + 1.0, fx(frac) - x0, 9.0), kBarPix);
        p.setPen(QPen(kPix, 1.2));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(fx(peakFrac) - 2.0, yRail + 1.0, 3.0, 9.0));
    }
    p.setPen(QPen(kPix, 2));
    p.drawLine(QPointF(x0, yRail), QPointF(x1, yRail));
    const auto tick = [&](double fr, bool major) {
        p.setPen(QPen(kPix, major ? 1.8 : 1.0));
        p.drawLine(QPointF(fx(fr), yRail),
                   QPointF(fx(fr), yRail + (major ? 12.0 : 6.0)));
    };
    if (rx) {
        for (int s = 1; s <= 9; ++s) tick(scaleFrac((s - 9) * 6.0), s & 1);
        for (int over = 20; over <= 60; over += 20) tick(scaleFrac(over), true);
    } else {
        for (int w = 0; w <= 120; w += 10) tick(w / kMaxW, w % 50 == 0);
    }
    tick(1.0, true);                          // end bracket, full drop

    // Chunky readout, lower left — the 588's blocky screen font.
    QFont f("DejaVu Sans Mono");
    f.setPixelSize(15);
    f.setBold(true);
    p.setFont(f);
    p.setPen(kPix);
    const QRectF row(x0, height() - 22.0, x1 - x0, 18.0);
    if (rx) {
        p.drawText(row, Qt::AlignLeft | Qt::AlignVCenter, sUnitsText(displayDb()));
        if (mode_ != Sig) {
            f.setPixelSize(10); p.setFont(f);
            p.setPen(kCyan);
            p.drawText(row, Qt::AlignRight | Qt::AlignVCenter,
                       mode_ == Avg ? "AVG" : "PK");
        }
    } else {
        p.drawText(row, Qt::AlignLeft | Qt::AlignVCenter,
                   QString("%1W").arg(fwdW_, 0, 'f', 0));
        p.setPen(swr_ > 2.5 ? QColor(235, 70, 50) : kYellow);
        p.drawText(row, Qt::AlignRight | Qt::AlignVCenter,
                   QString("SWR %1").arg(swr_, 0, 'f', 1));
    }
    // Screen caption, cyan, top right — names the connected radio
    // (nominative use, same footing as the gold face's wordmark).
    QFont fc("DejaVu Sans");
    fc.setPixelSize(8);
    fc.setBold(true);
    fc.setLetterSpacing(QFont::AbsoluteSpacing, 1.0);
    p.setFont(fc);
    p.setPen(kCyan);
    p.drawText(QRectF(0, 4, width() - 12, 10), Qt::AlignRight, wordmark_);
    drawSdrTag(p, width() / 2.0 - 12, height() - 20.0, kCyan);
}

} // namespace ttc
