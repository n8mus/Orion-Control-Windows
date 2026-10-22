// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/VoiceEqDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QTabWidget>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QInputDialog>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QWheelEvent>
#include <QSettings>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <limits>

namespace ttc {

namespace {
constexpr float kLoHz = 40.f, kHiHz = 6000.f;
constexpr float kDbSpan = 15.f;                    // plot is ±15 dB

const char* kStyle =
    "QDialog { background: #141b24; }"
    "QLabel { color: #8fa3b8; font-size: 12px; }"
    "QCheckBox { color: #c8d4e0; }"
    "QCheckBox::indicator { width: 13px; height: 13px; border: 1px solid #4a5e76;"
    " background: #1c2430; border-radius: 2px; }"
    "QCheckBox::indicator:hover { border-color: #6a86a6; }"
    "QCheckBox::indicator:checked { background: #e8b04a; border-color: #e8b04a; }"
    "QCheckBox::indicator:disabled { border-color: #2a3644; }"
    "QCheckBox::indicator:checked:disabled { background: #7a6230; border-color: #7a6230; }"
    // Group headings carry their own colour: unstyled, they take the
    // desktop theme's (live-found on Cinnamon: dark grey on the dark
    // dialog, unreadable).
    "QGroupBox { color: #e8b04a; font-weight: bold; border: 1px solid #2a3644;"
    " margin-top: 12px; padding-top: 6px; }"
    "QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; }"
    "QTabWidget::pane, QTabWidget > QWidget > QWidget { background: #141b24; }"
    "QComboBox, QPushButton { background: #1c2430; color: #c8d4e0;"
    " border: 1px solid #2a3644; padding: 3px 8px; }"
    "QPushButton:default { border-color: #3ecf7a; }"
    // Colors only on the spin boxes: a border rule drops the arrows.
    "QSpinBox, QDoubleSpinBox { background: #1c2430; color: #c8d4e0; }"
    "QPushButton:hover, QComboBox:hover { border-color: #3d5068; }"
    // The open list: unstyled it took the desktop theme — pale grey text on
    // white, unreadable (operator, 2026-10-16).
    "QComboBox QAbstractItemView { background: #1c2430; color: #c8d4e0;"
    " border: 1px solid #3d5068; outline: 0;"
    " selection-background-color: #2b5c8a; selection-color: #ffffff; }"
    "QComboBox::drop-down { border: none; width: 18px; }"
    "QComboBox::down-arrow { image: url(:/combo-down.png); width: 10px; height: 6px; }"
    "QComboBox::down-arrow:disabled { image: url(:/combo-down-off.png); }"
    // A section that is switched off still reads: dimmer, not invisible.
    "QPushButton:disabled { color: #66768a; border-color: #232c38; }"
    "QSpinBox:disabled, QDoubleSpinBox:disabled, QComboBox:disabled"
    " { color: #6f7f92; background: #18202a; }";
}  // namespace

VoiceEqCurve::VoiceEqCurve(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(170);
    setMouseTracking(true);                        // hover highlight + cursor
}

QRectF VoiceEqCurve::plot() const { return QRectF(rect()).adjusted(34, 8, -8, -20); }

float VoiceEqCurve::hzAt(double x) const {
    const QRectF r = plot();
    const double t = std::clamp((x - r.left()) / r.width(), 0.0, 1.0);
    return float(kLoHz * std::pow(double(kHiHz / kLoHz), t));
}

float VoiceEqCurve::dbAt(double y) const {
    const QRectF r = plot();
    return float((r.center().y() - y) / (r.height() / 2) * kDbSpan);
}

// Peaks and shelves sit at their gain; high/low-pass sit on the total
// curve at their corner, where the eye looks for them.
QPointF VoiceEqCurve::handlePos(int i) const {
    const QRectF r = plot();
    const auto& b = s_.bands[size_t(i)];
    const double x = r.left() + r.width() * std::log(std::clamp(b.freqHz, kLoHz, kHiHz) / kLoHz)
                                / std::log(kHiHz / kLoHz);
    const auto t = VoiceEqSettings::slotType(i);
    const bool slope = t == ClientEq::FilterType::HighPass || t == ClientEq::FilterType::LowPass;
    VoiceEqSettings on = s_;
    on.enabled = true;
    const float db = slope ? on.responseDb(b.freqHz) : (b.enabled ? b.gainDb : 0.f);
    const double y = r.center().y() - std::clamp(db, -kDbSpan, kDbSpan) / kDbSpan * r.height() / 2;
    return {x, y};
}

int VoiceEqCurve::hitTest(const QPointF& p) const {
    int best = -1;
    double bestD = 10.0;                           // px grab radius
    for (int i = 0; i < VoiceEqSettings::kBands; ++i) {
        const double d = std::hypot(handlePos(i).x() - p.x(), handlePos(i).y() - p.y());
        if (d < bestD) { bestD = d; best = i; }
    }
    return best;
}

void VoiceEqCurve::emitBand(int band, ClientEq::BandParams p) {
    // The band rows' own limits, so the spin boxes never clamp under us.
    p.freqHz = std::clamp(std::round(p.freqHz / 5.f) * 5.f, 20.f, 8000.f);
    p.gainDb = std::clamp(std::round(p.gainDb * 2.f) / 2.f, -15.f, 15.f);
    p.q = std::clamp(std::round(p.q * 100.f) / 100.f, 0.3f, 8.f);
    s_.bands[size_t(band)] = p;
    if (onBandEdited) onBandEdited(band, p);
    update();
}

void VoiceEqCurve::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    drag_ = hitTest(e->position());
    if (drag_ < 0) return;
    dragQ_ = e->modifiers() & Qt::ShiftModifier;
    dragStart_ = e->position();
    dragStartQ_ = s_.bands[size_t(drag_)].q;
    ClientEq::BandParams b = s_.bands[size_t(drag_)];
    if (!b.enabled || !s_.enabled) {               // grabbing it means using it
        b.enabled = true;
        s_.enabled = true;
        emitBand(drag_, b);
    }
    setCursor(Qt::ClosedHandCursor);
}

void VoiceEqCurve::mouseMoveEvent(QMouseEvent* e) {
    if (drag_ < 0) {
        const int h = hitTest(e->position());
        if (h != hover_) { hover_ = h; update(); }
        setCursor(h >= 0 ? Qt::OpenHandCursor : Qt::ArrowCursor);
        return;
    }
    ClientEq::BandParams b = s_.bands[size_t(drag_)];
    const auto t = VoiceEqSettings::slotType(drag_);
    const bool slope = t == ClientEq::FilterType::HighPass || t == ClientEq::FilterType::LowPass;
    if (dragQ_ && !slope) {
        // Shift-drag: Q, exponential so half the height spans a decade.
        const double dy = dragStart_.y() - e->position().y();
        b.q = float(dragStartQ_ * std::pow(10.0, dy / std::max(1.0, plot().height() / 2)));
    } else {
        b.freqHz = hzAt(e->position().x());
        if (!slope) b.gainDb = dbAt(e->position().y());
    }
    emitBand(drag_, b);
}

void VoiceEqCurve::mouseReleaseEvent(QMouseEvent*) {
    if (drag_ < 0) return;
    drag_ = -1;
    setCursor(Qt::OpenHandCursor);
    update();
    if (onDragFinished) onDragFinished();
}

void VoiceEqCurve::wheelEvent(QWheelEvent* e) {
    const int band = drag_ >= 0 ? drag_ : hitTest(e->position());
    const auto t = band >= 0 ? VoiceEqSettings::slotType(band) : ClientEq::FilterType::Peak;
    if (band < 0 || t == ClientEq::FilterType::HighPass || t == ClientEq::FilterType::LowPass)
        return;
    ClientEq::BandParams b = s_.bands[size_t(band)];
    b.q *= float(std::pow(1.1, e->angleDelta().y() / 120.0));   // ~10 % a notch
    emitBand(band, b);
    e->accept();
}

void VoiceEqCurve::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(34, 8, -8, -20);
    p.fillRect(rect(), QColor(11, 16, 22));
    const auto xOf = [&](float hz) {
        return r.left() + r.width() * std::log(hz / kLoHz) / std::log(kHiHz / kLoHz);
    };
    const auto yOf = [&](float db) {
        return r.center().y() - std::clamp(db, -kDbSpan, kDbSpan) / kDbSpan * r.height() / 2;
    };
    // The radio's side of the story: above 3.9 kHz the Orion's TX filter
    // removes it whatever the EQ does.
    QFont f = p.font();
    f.setPixelSize(10);
    p.setFont(f);
    if (edgeHz_ > 0) {
        p.fillRect(QRectF(QPointF(xOf(edgeHz_), r.top()), r.bottomRight()),
                   QColor(60, 30, 30, 90));
        p.setPen(QColor(150, 90, 90));
        p.drawText(QRectF(xOf(edgeHz_) - 103, r.top() + 1, 100, 14),
                   Qt::AlignRight | Qt::AlignVCenter, edgeLabel_);
    }
    for (float hz : {50.f, 100.f, 200.f, 500.f, 1000.f, 2000.f, 5000.f}) {
        const double x = xOf(hz);
        p.setPen(QColor(32, 42, 54));
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        p.setPen(QColor(110, 125, 140));
        p.drawText(QRectF(x - 20, r.bottom() + 3, 40, 14), Qt::AlignCenter,
                   hz >= 1000 ? QString("%1k").arg(hz / 1000) : QString::number(hz));
    }
    for (int db = -12; db <= 12; db += 6) {
        const double y = yOf(float(db));
        p.setPen(db == 0 ? QColor(70, 84, 100) : QColor(32, 42, 54));
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
        p.setPen(QColor(110, 125, 140));
        p.drawText(QRectF(0, y - 7, 30, 14), Qt::AlignRight | Qt::AlignVCenter,
                   QString::number(db));
    }
    constexpr int kPts = 240;
    const auto& targets = voiceEqTargets();
    if (target_ > 0 && target_ < int(targets.size())) {
        const VoiceEqTarget& t = targets[size_t(target_)];
        QPainterPath tp;
        for (int i = 0; i <= kPts; ++i) {
            const float hz = kLoHz * std::pow(kHiHz / kLoHz, float(i) / kPts);
            const QPointF pt(xOf(hz), yOf(t.dbAt(hz)));
            if (i == 0) tp.moveTo(pt); else tp.lineTo(pt);
        }
        p.setPen(QPen(QColor(232, 176, 74, 200), 1.5, Qt::DashLine));
        p.drawPath(tp);
        p.drawText(QRectF(r.left() + 4, r.top() + 1, 200, 14),
                   Qt::AlignLeft | Qt::AlignVCenter, "target: " + t.name);
    }
    QPainterPath path;
    for (int i = 0; i <= kPts; ++i) {
        const float hz = kLoHz * std::pow(kHiHz / kLoHz, float(i) / kPts);
        const QPointF pt(xOf(hz), yOf(s_.responseDb(hz)));
        if (i == 0) path.moveTo(pt); else path.lineTo(pt);
    }
    p.setPen(QPen(s_.enabled ? QColor(80, 230, 120) : QColor(90, 100, 110), 2));
    p.drawPath(path);

    // Band handles: numbered dots, filled when the band is on, hollow when
    // off; the one under the mouse or in hand is ringed, with a readout.
    QFont hf = p.font();
    hf.setPixelSize(10);
    hf.setBold(true);
    p.setFont(hf);
    for (int i = 0; i < VoiceEqSettings::kBands; ++i) {
        const auto& b = s_.bands[size_t(i)];
        const QPointF c = handlePos(i);
        const bool live = s_.enabled && b.enabled;
        const bool hot = i == drag_ || i == hover_;
        p.setPen(QPen(live ? QColor(232, 176, 74) : QColor(110, 125, 140), hot ? 2.5 : 1.5));
        p.setBrush(live ? QColor(232, 176, 74, hot ? 255 : 190) : QColor(20, 27, 36));
        p.drawEllipse(c, hot ? 8.5 : 7.0, hot ? 8.5 : 7.0);
        p.setPen(live ? QColor(20, 27, 36) : QColor(150, 165, 180));
        // Labelled like the band rows: HP, LS, peaks 1-4, HS, LP.
        static const char* kTag[VoiceEqSettings::kBands] =
            {"HP", "LS", "1", "2", "3", "4", "HS", "LP"};
        QFont tf = hf;
        tf.setPixelSize(i == 0 || i == 1 || i >= 6 ? 8 : 10);
        p.setFont(tf);
        p.drawText(QRectF(c.x() - 7, c.y() - 7, 14, 14), Qt::AlignCenter, kTag[i]);
        p.setFont(hf);
        if (hot) {
            const auto t = VoiceEqSettings::slotType(i);
            const bool slope = t == ClientEq::FilterType::HighPass
                            || t == ClientEq::FilterType::LowPass;
            const QString txt = slope
                ? QString("%1  %2 Hz").arg(VoiceEqSettings::slotName(i)).arg(b.freqHz, 0, 'f', 0)
                : QString("%1  %2 Hz  %3 dB  Q %4").arg(VoiceEqSettings::slotName(i))
                      .arg(b.freqHz, 0, 'f', 0).arg(b.gainDb, 0, 'f', 1).arg(b.q, 0, 'f', 2);
            const QRectF box(std::min(c.x() + 10, r.right() - 210.0), std::max(c.y() - 22, r.top()), 210, 15);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(11, 16, 22, 220));
            p.drawRect(box);
            p.setPen(QColor(232, 176, 74));
            p.drawText(box.adjusted(4, 0, 0, 0), Qt::AlignVCenter, txt);
        }
    }
}

LiveMeter::LiveMeter(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(112);
}

void LiveMeter::setMarks(float thresholdDb, float ceilingDb) {
    thrDb_ = thresholdDb;
    ceilDb_ = ceilingDb;
    update();
}

void LiveMeter::setLevels(const LiveLevels& l) {
    // Peak hold with a ~20 dB/s fall (called every ~100 ms).
    const auto fall = [](float shown, float now) {
        return now >= shown ? now : std::max(now, shown - 2.0f);
    };
    inShown_ = l.live ? fall(inShown_, l.inDb) : -120.f;
    outShown_ = l.live ? fall(outShown_, l.outDb) : -120.f;
    compShown_ = l.live ? (l.compDb <= compShown_ ? l.compDb
                                                  : std::min(0.f, compShown_ + 2.0f))
                        : 0.f;
    l_ = l;
    update();
}

void LiveMeter::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(11, 16, 22));
    QFont f = p.font();
    f.setPixelSize(11);
    p.setFont(f);
    constexpr float kLo = -60.f;                   // bar scale: -60 .. 0 dBFS
    const int labelW = 92, valueW = 64;
    const QRectF barArea(labelW, 0, width() - labelW - valueW - 8, height());
    const auto xOf = [&](float db) {
        return barArea.left() + barArea.width() * (std::clamp(db, kLo, 0.f) - kLo) / -kLo;
    };
    const auto row = [&](int i) { return 8.0 + i * 20.0; };
    const QColor lbl(143, 163, 184), val(200, 212, 224), dim(90, 104, 120);
    const auto label = [&](int i, const QString& t) {
        p.setPen(lbl);
        p.drawText(QRectF(8, row(i), labelW - 12, 14), Qt::AlignVCenter, t);
    };
    const auto value = [&](int i, const QString& t) {
        p.setPen(val);
        p.drawText(QRectF(width() - valueW - 4, row(i), valueW, 14),
                   Qt::AlignRight | Qt::AlignVCenter, t);
    };
    const auto bar = [&](int i, float db, QColor c) {
        const QRectF track(barArea.left(), row(i) + 2, barArea.width(), 10);
        p.fillRect(track, QColor(28, 36, 48));
        if (db > kLo) p.fillRect(QRectF(track.left(), track.top(), xOf(db) - track.left(), 10), c);
    };
    if (!l_.live) {
        p.setPen(dim);
        p.drawText(rect(), Qt::AlignCenter, "LIVE is off — turn it on (TX bar) to see live levels");
        return;
    }
    const QColor green(29, 158, 117), amber(232, 176, 74), red(226, 75, 74), purple(127, 119, 221);
    label(0, "Mic in");
    bar(0, inShown_, green);
    value(0, QString("%1 dB").arg(inShown_, 0, 'f', 0));
    label(1, "To SignaLink");
    // Under the SignaLink's pickup threshold it mutes its TX audio: show
    // the bar amber below it, green above.
    const bool knownThr = !std::isnan(thrDb_);
    bar(1, outShown_, knownThr && outShown_ < thrDb_ ? amber : green);
    value(1, QString("%1 dB").arg(outShown_, 0, 'f', 0));
    const auto mark = [&](float db, QColor c) {
        const double x = xOf(db);
        p.fillRect(QRectF(x - 1, row(1) - 2, 2, 18), c);
    };
    if (knownThr) mark(thrDb_, amber);
    mark(ceilDb_, red);
    label(2, "Compressor");
    {
        const QRectF track(barArea.left(), row(2) + 3, barArea.width(), 8);
        p.fillRect(track, QColor(28, 36, 48));
        const double w = track.width() * std::clamp(-compShown_, 0.f, 20.f) / 20.f;
        p.fillRect(QRectF(track.right() - w, track.top(), w, 8), purple);  // GR grows leftward
    }
    value(2, QString("%1 dB").arg(compShown_, 0, 'f', 0));
    label(3, "Gate");
    p.setPen(!l_.gateOn ? dim : (l_.gateOpen ? green : amber));
    p.drawText(QRectF(barArea.left(), row(3), 200, 14), Qt::AlignVCenter,
               !l_.gateOn ? "off" : (l_.gateOpen ? "open — passing audio" : "closed — between words"));
    label(4, "Delay");
    p.setPen(val);
    p.drawText(QRectF(barArea.left(), row(4), 200, 14), Qt::AlignVCenter,
               QString("%1 ms queued").arg(l_.delayMs, 0, 'f', 0));
    p.setPen(dim);
    p.drawText(QRectF(barArea.left() + 210, row(4), barArea.width() - 210, 14),
               Qt::AlignRight | Qt::AlignVCenter,
               knownThr ? "amber = SignaLink pickup  ·  red = ceiling"
                        : "red = ceiling  ·  pickup not measured yet");
}

VoiceEqDialog::VoiceEqDialog(const VoiceEqSettings& s, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle("Voice keyer audio — EQ & dynamics");
    setStyleSheet(kStyle);
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(12, 10, 12, 10);
    lay->setSpacing(8);

    // Profiles: the built-in starting points, then the operator's own.
    // Save as… stores the curve on screen under a name; Delete removes one
    // of his (built-ins stay). The curve on screen is ALWAYS saved anyway
    // (live apply) — profiles are for keeping and switching between curves.
    auto* prof = new QHBoxLayout;
    prof->addWidget(new QLabel("Profile", this));
    preset_ = new QComboBox(this);
    preset_->setMinimumWidth(220);
    preset_->setToolTip("Built-in starting points, then your saved profiles.\n"
                        "Tune by ear: listen here, then on the Orion's MON\n"
                        "into a dummy load or at low power.");
    prof->addWidget(preset_, 1);
    auto* save = new QPushButton("Save", this);
    save->setToolTip("Save your changes into the selected profile (a built-in\n"
                     "asks for a name for your version — those stay as shipped)");
    auto* saveAs = new QPushButton("Save as…", this);
    saveAs->setToolTip("Save the settings on screen as a new profile");
    auto* del = new QPushButton("Delete", this);
    delBtn_ = del;
    del->setToolTip("Delete the selected profile (your own only)");
    prof->addWidget(save);
    prof->addWidget(saveAs);
    prof->addWidget(del);
    lay->addLayout(prof);
    fillProfiles(QSettings().value("vk/eqProfile").toString());
    // Edits keep the profile's name, marked "(modified)" until saved
    // (operator's ask: tweak the profile he is on, not a nameless curve).
    const bool wasModified = QSettings().value("vk/eqProfileModified", false).toBool();
    modified_ = false;                             // so the mark is drawn now
    setModified(wasModified);
    const auto syncDelete = [this, del] {
        del->setEnabled(preset_->currentData().toString().startsWith("u:"));
    };
    syncDelete();
    connect(preset_, &QComboBox::currentIndexChanged, this, syncDelete);

    // EQ tab: on/off + target, the curve, the eight bands.
    auto* tabs = new QTabWidget(this);
    tabs->setStyleSheet(
        "QTabWidget::pane { border: 1px solid #2a3644; }"
        "QTabBar::tab { background: #1c2430; color: #8fa3b8; padding: 4px 14px;"
        " border: 1px solid #2a3644; }"
        "QTabBar::tab:selected { background: #26303e; color: #e8b04a; }");
    auto* eqPage = new QWidget(tabs);
    auto* eqLay = new QVBoxLayout(eqPage);
    eqLay->setContentsMargins(8, 8, 8, 8);
    auto* top = new QHBoxLayout;
    enabled_ = new QCheckBox("EQ on", eqPage);
    top->addWidget(enabled_);
    top->addStretch(1);
    top->addWidget(new QLabel("Target", eqPage));
    target_ = new QComboBox(eqPage);
    for (const VoiceEqTarget& t : voiceEqTargets()) target_->addItem(t.name);
    target_->setToolTip("A reference curve drawn behind yours (dashed) to\n"
                        "shape toward — display only. From AetherSDR.");
    top->addWidget(target_);
    eqLay->addLayout(top);
    curve_ = new VoiceEqCurve(eqPage);
    eqLay->addWidget(curve_);

    auto* g = new QGridLayout;
    g->setHorizontalSpacing(10);
    g->addWidget(new QLabel("Band", this), 0, 0);
    g->addWidget(new QLabel("Hz", this), 0, 1);
    g->addWidget(new QLabel("Gain dB", this), 0, 2);
    g->addWidget(new QLabel("Q / slope", this), 0, 3);
    for (int i = 0; i < VoiceEqSettings::kBands; ++i) {
        Row& row = rows_[i];
        const ClientEq::FilterType t = VoiceEqSettings::slotType(i);
        const bool slopeBand = t == ClientEq::FilterType::HighPass
                            || t == ClientEq::FilterType::LowPass;
        row.on = new QCheckBox(VoiceEqSettings::slotName(i), this);
        g->addWidget(row.on, i + 1, 0);
        row.hz = new QSpinBox(this);
        row.hz->setRange(20, 8000);
        row.hz->setSingleStep(10);
        row.hz->setFixedWidth(90);
        g->addWidget(row.hz, i + 1, 1);
        if (slopeBand) {
            row.slope = new QComboBox(this);
            for (int sl : {12, 24, 36, 48})
                row.slope->addItem(QString("%1 dB/oct").arg(sl), sl);
            g->addWidget(row.slope, i + 1, 3);
        } else {
            row.db = new QDoubleSpinBox(this);
            row.db->setRange(-15.0, 15.0);
            row.db->setDecimals(1);
            row.db->setSingleStep(0.5);
            row.db->setFixedWidth(80);
            g->addWidget(row.db, i + 1, 2);
            row.q = new QDoubleSpinBox(this);
            row.q->setRange(0.3, 8.0);
            row.q->setDecimals(2);
            row.q->setSingleStep(0.1);
            row.q->setFixedWidth(80);
            g->addWidget(row.q, i + 1, 3);
        }
    }
    g->setColumnStretch(4, 1);
    eqLay->addLayout(g);
    tabs->addTab(eqPage, "EQ");
    tabs->addTab(buildDynamicsPage(), "Dynamics");
    lay->addWidget(tabs);

    // Live levels + the SignaLink pickup finder.
    meter_ = new LiveMeter(this);
    lay->addWidget(meter_);
    auto* calRow = new QHBoxLayout;
    calBtn_ = new QPushButton("Find SignaLink pickup…", this);
    calBtn_->setToolTip("Plays a slowly rising 1 kHz tone into the SignaLink.\n"
                        "Click \"Light is on\" the moment its PTT light comes\n"
                        "on — that level becomes the amber mark. Don't key\n"
                        "the radio while it runs (the tone would go out).");
    calMark_ = new QPushButton("Light is on", this);
    calMark_->setEnabled(false);
    calText_ = new QLabel(this);
    calRow->addWidget(calBtn_);
    calRow->addWidget(calMark_);
    calRow->addWidget(calText_, 1);
    lay->addLayout(calRow);
    connect(calBtn_, &QPushButton::clicked, this, [this] {
        if (calRunning_) emit calibrateCancelRequested();
        else emit calibrateStartRequested();
    });
    connect(calMark_, &QPushButton::clicked, this, [this] { emit calibrateMarkRequested(); });

    status_ = new QLabel(this);
    status_->setWordWrap(true);
    lay->addWidget(status_);

    auto* bRow = new QHBoxLayout;
    bRow->addWidget(new QLabel("Listen to", this));
    slot_ = new QComboBox(this);
    for (int i = 1; i <= 4; ++i) slot_->addItem(QString("VK%1").arg(i), i - 1);
    bRow->addWidget(slot_);
    auto* listen = new QPushButton("Listen", this);
    listen->setToolTip("Play the chosen message on the PC speakers with the\n"
                       "current curve (nothing keyed). Changes apply by\n"
                       "themselves — the VK buttons always send the curve\n"
                       "shown here.");
    listen->setDefault(true);
    bRow->addWidget(listen);
    bRow->addStretch(1);
    auto* close = new QPushButton("Close", this);
    bRow->addWidget(close);
    lay->addLayout(bRow);

    target_->setCurrentIndex(std::clamp(
        QSettings().value("vk/eqTarget", 0).toInt(), 0, target_->count() - 1));
    curve_->setTarget(target_->currentIndex());
    connect(target_, &QComboBox::currentIndexChanged, this, [this](int i) {
        curve_->setTarget(i);
        QSettings().setValue("vk/eqTarget", i);
    });

    applyTimer_ = new QTimer(this);
    applyTimer_->setSingleShot(true);
    applyTimer_->setInterval(700);                 // spin-box scrubbing settles
    connect(applyTimer_, &QTimer::timeout, this,
            [this] { emit applyRequested(collect()); });

    load(s);

    // Any edit redraws the curve at once, marks the profile "(modified)"
    // (its name stays — Save writes the edits back into it), and re-renders
    // shortly after.
    const auto edited = [this] {
        if (loading_) return;
        refreshEnables();
        curve_->setSettings(collect());
        setModified(true);
        emit liveSettingsChanged(collect());       // live mic: at once
        applyTimer_->start();                      // VK messages: when settled
    };
    connect(enabled_, &QCheckBox::toggled, this, edited);
    // Curve drags write the band rows (quietly) and run one edit — the
    // same path as typing the numbers.
    curve_->onBandEdited = [this, edited](int i, const ClientEq::BandParams& b) {
        loading_ = true;
        enabled_->setChecked(enabled_->isChecked() || b.enabled);
        Row& row = rows_[i];
        row.on->setChecked(b.enabled);
        row.hz->setValue(int(std::lround(b.freqHz)));
        if (row.db) row.db->setValue(b.gainDb);
        if (row.q) row.q->setValue(b.q);
        loading_ = false;
        edited();
    };
    for (QCheckBox* c : {dyn_.gateOn, dyn_.deessOn, dyn_.compOn, dyn_.limOn, dyn_.limDc})
        connect(c, &QCheckBox::toggled, this, edited);
    for (QDoubleSpinBox* d : {dyn_.gateThr, dyn_.gateRatio, dyn_.gateFloor,
                              dyn_.gateHold, dyn_.gateRel, dyn_.deHz, dyn_.deThr,
                              dyn_.deAmt, dyn_.cThr, dyn_.cRatio, dyn_.cAtt,
                              dyn_.cRel, dyn_.cKnee, dyn_.cDrive, dyn_.cMakeup,
                              dyn_.limCeil})
        connect(d, &QDoubleSpinBox::valueChanged, this, edited);
    connect(dyn_.cPhase, &QSpinBox::valueChanged, this, edited);
    for (Row& row : rows_) {
        connect(row.on, &QCheckBox::toggled, this, edited);
        connect(row.hz, &QSpinBox::valueChanged, this, edited);
        if (row.db) connect(row.db, &QDoubleSpinBox::valueChanged, this, edited);
        if (row.q) connect(row.q, &QDoubleSpinBox::valueChanged, this, edited);
        if (row.slope)
            connect(row.slope, &QComboBox::currentIndexChanged, this, edited);
    }
    // activated, not currentIndexChanged: picking the profile already shown
    // reloads its saved version — the way to drop unsaved edits.
    connect(preset_, &QComboBox::activated, this, [this](int idx) {
        const QString key = preset_->itemData(idx).toString();
        VoiceEqSettings s;
        if (key.startsWith("b:"))
            s = VoiceEqSettings::preset(VoiceEqSettings::Preset(key.mid(2).toInt()));
        else if (!key.startsWith("u:")
                 || !VoiceEqSettings::loadUserProfile(key.mid(2), s))
            return;                                // "—" or vanished
        load(s);
        setActiveProfile(key);
        setModified(false);
        emit liveSettingsChanged(collect());
        applyTimer_->start();
    });
    connect(save, &QPushButton::clicked, this, [this] {
        const QString key = preset_->currentData().toString();
        if (!key.startsWith("u:")) { saveAsPrompt(); return; }   // built-in / none
        VoiceEqSettings::saveUserProfile(key.mid(2), collect());
        setModified(false);
        setStatus(QString("Saved \"%1\".").arg(key.mid(2)));
    });
    connect(saveAs, &QPushButton::clicked, this, [this] { saveAsPrompt(); });
    connect(del, &QPushButton::clicked, this, [this] {
        const QString key = preset_->currentData().toString();
        if (!key.startsWith("u:")) return;
        if (QMessageBox::question(this, "Delete EQ profile",
                QString("Delete your profile \"%1\"? (The curve on screen "
                        "stays.)").arg(key.mid(2))) != QMessageBox::Yes)
            return;
        VoiceEqSettings::deleteUserProfile(key.mid(2));
        fillProfiles(QString());
        setActiveProfile(QString());
        setModified(false);
        setStatus(QString("Deleted profile \"%1\".").arg(key.mid(2)));
    });
    connect(listen, &QPushButton::clicked, this, [this] {
        applyTimer_->stop();                       // listen applies it now
        emit listenRequested(slot_->currentData().toInt(), collect());
    });
    connect(close, &QPushButton::clicked, this, &QDialog::close);
}

void VoiceEqDialog::setStatus(const QString& text) { status_->setText(text); }

void VoiceEqDialog::setLiveLevels(const LiveLevels& l) { meter_->setLevels(l); }

void VoiceEqDialog::setPickupThreshold(float db) {
    meter_->setMarks(db, collect().limit.on ? collect().limit.ceilingDb : 0.f);
}

void VoiceEqDialog::setCalibration(bool running, const QString& text) {
    calRunning_ = running;
    calBtn_->setText(running ? "Stop" : "Find SignaLink pickup…");
    calMark_->setEnabled(running);
    calText_->setText(text);
}

// Item data: "" = none/edited, "b:<n>" = built-in preset n, "u:<name>" =
// the operator's profile.
void VoiceEqDialog::fillProfiles(const QString& select) {
    const QSignalBlocker b(preset_);
    preset_->clear();
    preset_->addItem("—  (your current curve)", QString());
    const QStringList builtins = VoiceEqSettings::presetNames();
    for (int i = 0; i < builtins.size(); ++i)
        preset_->addItem(builtins[i], QString("b:%1").arg(i));
    const QStringList mine = VoiceEqSettings::userProfileNames();
    if (!mine.isEmpty()) preset_->insertSeparator(preset_->count());
    for (const QString& n : mine) preset_->addItem("★ " + n, "u:" + n);
    preset_->setCurrentIndex(std::max(0, preset_->findData(select)));
    if (delBtn_) delBtn_->setEnabled(preset_->currentData().toString().startsWith("u:"));
}

void VoiceEqDialog::setActiveProfile(const QString& key) {
    QSettings().setValue("vk/eqProfile", key);
}

// The shown profile's name gets " (modified)" while the settings on screen
// differ from what it holds; the item's real name is restored by refilling.
void VoiceEqDialog::setModified(bool m) {
    if (m && modified_) return;                    // already marked (drags fire a lot)
    modified_ = m;
    QSettings().setValue("vk/eqProfileModified", m);
    const int idx = preset_->currentIndex();
    const QString key = preset_->currentData().toString();
    fillProfiles(key);                             // clean names first
    if (m && !key.isEmpty() && idx >= 0)
        preset_->setItemText(preset_->currentIndex(),
                             preset_->currentText() + "  (modified)");
}

void VoiceEqDialog::saveAsPrompt() {
    const QString cur = preset_->currentData().toString();
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, "Save EQ profile", "Profile name:", QLineEdit::Normal,
        cur.startsWith("u:") ? cur.mid(2) : QString(), &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    if (VoiceEqSettings::userProfileNames().contains(name)
        && QMessageBox::question(this, "Save EQ profile",
               QString("Replace your profile \"%1\"?").arg(name))
               != QMessageBox::Yes)
        return;
    VoiceEqSettings::saveUserProfile(name, collect());
    fillProfiles("u:" + name);
    setActiveProfile("u:" + name);
    setModified(false);
    setStatus(QString("Saved profile \"%1\".").arg(name));
}

namespace {
QDoubleSpinBox* dspin(QWidget* p, double lo, double hi, double step, int dec,
                      const QString& suffix, const QString& tip) {
    auto* d = new QDoubleSpinBox(p);
    d->setRange(lo, hi);
    d->setSingleStep(step);
    d->setDecimals(dec);
    d->setSuffix(suffix);
    d->setFixedWidth(100);
    d->setToolTip(tip);
    return d;
}
QGroupBox* stage(QWidget* p, const QString& title, QCheckBox*& on,
                 const QString& tip, QGridLayout*& g) {
    auto* box = new QGroupBox(title, p);
    g = new QGridLayout(box);
    g->setHorizontalSpacing(10);
    on = new QCheckBox("On", box);
    on->setToolTip(tip);
    g->addWidget(on, 0, 0);
    return box;
}
void field(QGridLayout* g, int row, int col, const QString& label, QWidget* w) {
    g->addWidget(new QLabel(label, w->parentWidget()), row, col * 2);
    g->addWidget(w, row, col * 2 + 1);
}
}  // namespace

// Dynamics tab, in chain order. Controls are the ported AetherSDR stages'
// own parameters; the rarely-touched ones (gate hysteresis/look-ahead,
// de-esser Q/slope, the compressor's internal limiter) keep their values
// from the loaded profile and are not shown.
QWidget* VoiceEqDialog::buildDynamicsPage() {
    auto* page = new QWidget(this);
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(8, 8, 8, 8);
    QGridLayout* g = nullptr;

    v->addWidget(stage(page, "NOISE GATE  (first)", dyn_.gateOn,
        "Turns the mic down between words — fan hum and room noise\n"
        "stay off the air. Ratio 2 = gentle expander, 10 = hard gate.", g));
    dyn_.gateThr   = dspin(page, -80, 0, 1, 0, " dB", "Opens above this level");
    dyn_.gateRatio = dspin(page, 1, 10, 0.5, 1, " :1", "2 = expander, 10 = gate");
    dyn_.gateFloor = dspin(page, -80, 0, 1, 0, " dB", "Most it ever turns down");
    dyn_.gateHold  = dspin(page, 0, 500, 10, 0, " ms", "Stays open this long after you stop");
    dyn_.gateRel   = dspin(page, 5, 2000, 10, 0, " ms", "How gently it closes");
    field(g, 1, 0, "Threshold", dyn_.gateThr);
    field(g, 1, 1, "Ratio", dyn_.gateRatio);
    field(g, 1, 2, "Floor", dyn_.gateFloor);
    field(g, 2, 0, "Hold", dyn_.gateHold);
    field(g, 2, 1, "Release", dyn_.gateRel);

    v->addWidget(stage(page, "DE-ESSER  (after the EQ)", dyn_.deessOn,
        "Cuts harsh 's' and 'sh' in their own band only — useful when\n"
        "the EQ boosts presence.", g));
    dyn_.deHz  = dspin(page, 1000, 12000, 100, 0, " Hz", "Sibilance band centre; SSB's is just under 3.9 kHz");
    dyn_.deThr = dspin(page, -60, 0, 1, 0, " dB", "Acts above this level in the band");
    dyn_.deAmt = dspin(page, -24, 0, 0.5, 1, " dB", "Most it cuts");
    field(g, 1, 0, "Frequency", dyn_.deHz);
    field(g, 1, 1, "Threshold", dyn_.deThr);
    field(g, 1, 2, "Max cut", dyn_.deAmt);

    v->addWidget(stage(page, "COMPRESSOR  (talk power)", dyn_.compOn,
        "Brings quiet syllables up toward loud ones — more average power\n"
        "at the same peak. Make-up gain is automatic for VK messages.", g));
    dyn_.cThr   = dspin(page, -60, 0, 1, 0, " dB", "Compresses above this level");
    dyn_.cRatio = dspin(page, 1, 20, 0.5, 1, " :1", "How hard (20 = limiter)");
    dyn_.cAtt   = dspin(page, 0.1, 200, 1, 1, " ms", "How fast it grabs (2-10 ms for voice)");
    dyn_.cRel   = dspin(page, 5, 2000, 10, 0, " ms", "How fast it lets go");
    dyn_.cKnee  = dspin(page, 0, 24, 1, 0, " dB", "Soft knee width (gentler onset)");
    dyn_.cDrive = dspin(page, 0, 18, 0.5, 1, " dB", "Pushes more voice into the compressor");
    dyn_.cPhase = new QSpinBox(page);
    dyn_.cPhase->setRange(0, 6);
    dyn_.cPhase->setFixedWidth(100);
    dyn_.cPhase->setToolTip("All-pass phase rotator stages (Orban/Optimod trick):\n"
                            "evens out lopsided voice peaks for 2-3 dB more\n"
                            "loudness, no tone change. 0 = off, 4 = broadcast");
    field(g, 1, 0, "Threshold", dyn_.cThr);
    field(g, 1, 1, "Ratio", dyn_.cRatio);
    field(g, 1, 2, "Knee", dyn_.cKnee);
    field(g, 2, 0, "Attack", dyn_.cAtt);
    field(g, 2, 1, "Release", dyn_.cRel);
    field(g, 2, 2, "Drive", dyn_.cDrive);
    dyn_.cMakeup = dspin(page, 0, 24, 0.5, 1, " dB",
        "LIVE mic only: gain after compression (VK messages measure\n"
        "their own). Raise until the voice sits just under the limiter\n"
        "— watch the Orion's ALC on peaks.");
    field(g, 3, 0, "Phase rot.", dyn_.cPhase);
    field(g, 3, 1, "Make-up (live)", dyn_.cMakeup);

    v->addWidget(stage(page, "FINAL LIMITER  (last)", dyn_.limOn,
        "The safety net: nothing leaves above this ceiling.", g));
    dyn_.limCeil = dspin(page, -12, 0, 0.5, 1, " dB", "Absolute ceiling");
    dyn_.limDc = new QCheckBox("Remove DC offset (25 Hz)", page);
    field(g, 1, 0, "Ceiling", dyn_.limCeil);
    g->addWidget(dyn_.limDc, 1, 2, 1, 3);
    v->addStretch(1);
    return page;
}

void VoiceEqDialog::load(const VoiceEqSettings& s) {
    loading_ = true;
    base_ = s;
    enabled_->setChecked(s.enabled);
    dyn_.gateOn->setChecked(s.gate.on);
    dyn_.gateThr->setValue(s.gate.thresholdDb);
    dyn_.gateRatio->setValue(s.gate.ratio);
    dyn_.gateFloor->setValue(s.gate.floorDb);
    dyn_.gateHold->setValue(s.gate.holdMs);
    dyn_.gateRel->setValue(s.gate.releaseMs);
    dyn_.deessOn->setChecked(s.deess.on);
    dyn_.deHz->setValue(s.deess.freqHz);
    dyn_.deThr->setValue(s.deess.thresholdDb);
    dyn_.deAmt->setValue(s.deess.amountDb);
    dyn_.compOn->setChecked(s.comp.on);
    dyn_.cThr->setValue(s.comp.thresholdDb);
    dyn_.cRatio->setValue(s.comp.ratio);
    dyn_.cAtt->setValue(s.comp.attackMs);
    dyn_.cRel->setValue(s.comp.releaseMs);
    dyn_.cKnee->setValue(s.comp.kneeDb);
    dyn_.cDrive->setValue(s.comp.driveDb);
    dyn_.cPhase->setValue(s.comp.phaseStages);
    dyn_.cMakeup->setValue(s.comp.makeupDb);
    dyn_.limOn->setChecked(s.limit.on);
    dyn_.limCeil->setValue(s.limit.ceilingDb);
    dyn_.limDc->setChecked(s.limit.dcBlock);
    for (int i = 0; i < VoiceEqSettings::kBands; ++i) {
        const auto& b = s.bands[size_t(i)];
        Row& row = rows_[i];
        row.on->setChecked(b.enabled);
        row.hz->setValue(int(std::lround(b.freqHz)));
        if (row.db) row.db->setValue(b.gainDb);
        if (row.q) row.q->setValue(b.q);
        if (row.slope)
            row.slope->setCurrentIndex(
                std::max(0, row.slope->findData(b.slopeDbPerOct)));
    }
    loading_ = false;
    refreshEnables();
    curve_->setSettings(collect());
}

void VoiceEqDialog::refreshEnables() {
    for (QWidget* w : std::initializer_list<QWidget*>{dyn_.gateThr, dyn_.gateRatio,
             dyn_.gateFloor, dyn_.gateHold, dyn_.gateRel})
        w->setEnabled(dyn_.gateOn->isChecked());
    for (QWidget* w : std::initializer_list<QWidget*>{dyn_.deHz, dyn_.deThr, dyn_.deAmt})
        w->setEnabled(dyn_.deessOn->isChecked());
    for (QWidget* w : std::initializer_list<QWidget*>{dyn_.cThr, dyn_.cRatio, dyn_.cAtt,
             dyn_.cRel, dyn_.cKnee, dyn_.cDrive, dyn_.cPhase, dyn_.cMakeup})
        w->setEnabled(dyn_.compOn->isChecked());
    dyn_.limCeil->setEnabled(dyn_.limOn->isChecked());
    dyn_.limDc->setEnabled(dyn_.limOn->isChecked());
    const bool eqOn = enabled_->isChecked();
    for (Row& row : rows_) {
        row.on->setEnabled(eqOn);
        const bool live = eqOn && row.on->isChecked();
        row.hz->setEnabled(live);
        if (row.db) row.db->setEnabled(live);
        if (row.q) row.q->setEnabled(live);
        if (row.slope) row.slope->setEnabled(live);
    }
}

VoiceEqSettings VoiceEqDialog::collect() const {
    VoiceEqSettings s = base_;                     // hidden fields ride along
    s.enabled = enabled_->isChecked();
    s.gate.on = dyn_.gateOn->isChecked();
    s.gate.thresholdDb = float(dyn_.gateThr->value());
    s.gate.ratio = float(dyn_.gateRatio->value());
    s.gate.floorDb = float(dyn_.gateFloor->value());
    s.gate.holdMs = float(dyn_.gateHold->value());
    s.gate.releaseMs = float(dyn_.gateRel->value());
    s.deess.on = dyn_.deessOn->isChecked();
    s.deess.freqHz = float(dyn_.deHz->value());
    s.deess.thresholdDb = float(dyn_.deThr->value());
    s.deess.amountDb = float(dyn_.deAmt->value());
    s.comp.on = dyn_.compOn->isChecked();
    s.comp.thresholdDb = float(dyn_.cThr->value());
    s.comp.ratio = float(dyn_.cRatio->value());
    s.comp.attackMs = float(dyn_.cAtt->value());
    s.comp.releaseMs = float(dyn_.cRel->value());
    s.comp.kneeDb = float(dyn_.cKnee->value());
    s.comp.driveDb = float(dyn_.cDrive->value());
    s.comp.phaseStages = dyn_.cPhase->value();
    s.comp.makeupDb = float(dyn_.cMakeup->value());
    s.limit.on = dyn_.limOn->isChecked();
    s.limit.ceilingDb = float(dyn_.limCeil->value());
    s.limit.dcBlock = dyn_.limDc->isChecked();
    for (int i = 0; i < VoiceEqSettings::kBands; ++i) {
        const Row& row = rows_[i];
        auto& b = s.bands[size_t(i)];
        b.type = VoiceEqSettings::slotType(i);
        b.enabled = row.on->isChecked();
        b.freqHz = float(row.hz->value());
        b.gainDb = row.db ? float(row.db->value()) : 0.f;
        b.q = row.q ? float(row.q->value()) : 0.707f;
        b.slopeDbPerOct = row.slope ? row.slope->currentData().toInt() : 12;
    }
    return s;
}

} // namespace ttc
