// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/DisplayPanel.h"

#include <QGridLayout>
#include <QSlider>
#include <QLabel>
#include <QComboBox>
#include <QCheckBox>
#include <QDir>
#include <QFileDialog>
#include <QLineEdit>
#include <QSettings>
#include <QSignalBlocker>
#include <algorithm>

namespace ttc {

namespace {
QLabel* makeCaption(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setStyleSheet("color: #8fa3b8; font-size: 13px; font-weight: bold;");
    return l;
}
} // namespace

// Sized ~150% (like the AUDIO popup): these get adjusted mid-operation, so
// bigger targets beat compactness.
DisplayPanel::DisplayPanel(QWidget* parent) : QWidget(parent) {
    setStyleSheet(
        "QWidget { background: #141b24; color: #c8d4e0; font-size: 14px; }"
        "QComboBox { background: #1c2430; border: 1px solid #2a3644; border-radius: 3px;"
        " padding: 4px 8px; }"
        "QComboBox QAbstractItemView { background: #1c2430; color: #c8d4e0;"
        " selection-background-color: #2a3644; }"
        "QCheckBox { spacing: 8px; }"
        "QCheckBox::indicator { width: 17px; height: 17px; }"
        "QLineEdit { background: #1c2430; border: 1px solid #2a3644; border-radius: 3px;"
        " padding: 4px 8px; }"
        "QSlider::groove:horizontal { height: 6px; background: #2a3644; border-radius: 3px; }"
        "QSlider::handle:horizontal { width: 18px; margin: -7px 0; border-radius: 9px;"
        " background: #6aa5d8; }");

    auto* g = new QGridLayout(this);
    g->setContentsMargins(16, 14, 16, 14);
    g->setHorizontalSpacing(12);
    g->setVerticalSpacing(11);

    // Reference level: the dB value at the TOP of the spectrum scale. Lower it
    // to pull weak signals up out of the floor, raise it for strong-signal work.
    g->addWidget(makeCaption("REF LVL", this), 0, 0);
    ref_ = new QSlider(Qt::Horizontal, this);
    ref_->setRange(-100, -20);
    ref_->setFixedWidth(225);
    refVal_ = new QLabel(this);
    refVal_->setFixedWidth(70);
    g->addWidget(ref_, 0, 1);
    g->addWidget(refVal_, 0, 2);

    // Range = scale height in dB (contrast): small range = punchy colors.
    g->addWidget(makeCaption("RANGE", this), 1, 0);
    range_ = new QSlider(Qt::Horizontal, this);
    range_->setRange(30, 120);
    range_->setFixedWidth(225);
    rangeVal_ = new QLabel(this);
    rangeVal_->setFixedWidth(70);
    g->addWidget(range_, 1, 1);
    g->addWidget(rangeVal_, 1, 2);

    g->addWidget(makeCaption("AVG", this), 2, 0);
    avg_ = new QComboBox(this);
    avg_->addItem("Off", 1);
    avg_->addItem("2", 2);
    avg_->addItem("4", 4);
    avg_->addItem("8", 8);
    avg_->addItem("16", 16);
    g->addWidget(avg_, 2, 1, 1, 2);

    g->addWidget(makeCaption("WF SPEED", this), 3, 0);
    speed_ = new QComboBox(this);
    speed_->addItem("Fast", 1);
    speed_->addItem("Normal", 2);
    speed_->addItem("Slow", 4);
    speed_->addItem("Slowest", 8);
    g->addWidget(speed_, 3, 1, 1, 2);

    g->addWidget(makeCaption("PALETTE", this), 4, 0);
    pal_ = new QComboBox(this);
    pal_->addItems(PanadapterWidget::paletteNames());
    g->addWidget(pal_, 4, 1, 1, 1);

    // Spectrum trace color rides next to the palette (same visual family).
    trace_ = new QComboBox(this);
    trace_->addItems({"Soft", "White", "Green", "Yellow", "Cyan"});
    trace_->setToolTip("Spectrum trace line color");
    g->addWidget(trace_, 4, 2, 1, 1);

    g->addWidget(makeCaption("BACKGND", this), 5, 0);
    bg_ = new QComboBox(this);
    bg_->addItems(PanadapterWidget::backgroundNames());
    g->addWidget(bg_, 5, 1, 1, 2);

    // Map brightness (only live for the map backdrops): separate day-side
    // and night-side levels, so the map can pop without drowning the trace.
    g->addWidget(makeCaption("MAP DAY", this), 6, 0);
    mapDay_ = new QSlider(Qt::Horizontal, this);
    mapDay_->setRange(30, 120);
    mapDay_->setFixedWidth(225);
    mapDayVal_ = new QLabel(this);
    mapDayVal_->setFixedWidth(70);
    g->addWidget(mapDay_, 6, 1);
    g->addWidget(mapDayVal_, 6, 2);

    g->addWidget(makeCaption("MAP NIGHT", this), 7, 0);
    mapNight_ = new QSlider(Qt::Horizontal, this);
    mapNight_->setRange(0, 60);
    mapNight_->setFixedWidth(225);
    mapNightVal_ = new QLabel(this);
    mapNightVal_->setFixedWidth(70);
    g->addWidget(mapNight_, 7, 1);
    g->addWidget(mapNightVal_, 7, 2);

    fill_ = new QCheckBox("Fill spectrum", this);
    peak_ = new QCheckBox("Peak hold", this);
    grid_ = new QCheckBox("Grid lines", this);
    solar_ = new QCheckBox("Solar data panel", this);
    solar_->setToolTip("SFI / sunspots / A / K / X-ray from NOAA, refreshed "
                       "every 15 minutes; the map backdrops also get the sun "
                       "marker with the same numbers");
    rose_ = new QCheckBox("Compass rose", this);
    rose_->setToolTip("Azimuthal world disc centered on your grid square.\n"
                      "Click a spot callsign to swing the pointer to that "
                      "station;\nclick inside the rose for a manual heading, "
                      "right-click clears.");
    plan_ = new QCheckBox("Band-plan shading", this);
    plan_->setToolTip("Tint the frequency scale: blue = CW/data segments, "
                      "green = phone (US allocations)");
    bigVfo_ = new QCheckBox("Large VFO digits", this);
    clock_ = new QCheckBox("Clock (radio panel)", this);
    cursor_ = new QCheckBox("Cursor line + zap preview", this);
    cursor_->setToolTip("Dashed line under the mouse with the exact "
                        "frequency,\nplus a green tick where a CW-zap "
                        "click would land");
    wfTime_ = new QCheckBox("Waterfall timestamps", this);
    wfTime_->setToolTip("UTC time labels down the waterfall's left edge —\n"
                        "read scrolled-back history like a log");
    zap_ = new QCheckBox("CW zap (snap to carrier)", this);
    zap_->setToolTip(
        "In CW modes a panadapter click snaps to the strongest signal\n"
        "within 150 Hz and puts the dial exactly on its carrier — the\n"
        "note lands right on your sidetone/SPOT pitch.\n"
        "Shift+click: tune exactly where clicked (no snap).\n"
        "Z key: zero-beat the strongest signal already in the passband.\n"
        "X key: CW⇄CWR flip — if the note's pitch doesn't change,\n"
        "you are perfectly zero-beat (by ear, no spectrum needed).");
    call_ = new QCheckBox("Callsign watermark", this);
    g->addWidget(fill_, 8, 0, 1, 3);
    g->addWidget(peak_, 9, 0, 1, 3);
    g->addWidget(grid_, 10, 0, 1, 3);
    g->addWidget(solar_, 11, 0, 1, 3);
    g->addWidget(rose_, 12, 0, 1, 3);
    g->addWidget(plan_, 13, 0, 1, 3);
    g->addWidget(bigVfo_, 14, 0, 1, 3);
    g->addWidget(clock_, 15, 0, 1, 3);
    g->addWidget(wfTime_, 16, 0, 1, 3);
    g->addWidget(cursor_, 17, 0, 1, 3);
    g->addWidget(zap_, 18, 0, 1, 3);
    g->addWidget(call_, 19, 0, 1, 3);

    g->addWidget(makeCaption("CALL", this), 20, 0);
    callEdit_ = new QLineEdit(this);
    callEdit_->setMaxLength(12);
    g->addWidget(callEdit_, 20, 1, 1, 2);

    // Station grid square: centers the compass rose (and any future
    // bearing/distance math). 4 or 6 characters.
    g->addWidget(makeCaption("GRID", this), 21, 0);
    gridEdit_ = new QLineEdit(this);
    gridEdit_->setMaxLength(6);
    gridEdit_->setToolTip("Your Maidenhead grid square (4 or 6 chars), e.g. EN82fq");
    g->addWidget(gridEdit_, 21, 1, 1, 2);

    auto updateLabels = [this] {
        refVal_->setText(QString("%1 dB").arg(ref_->value()));
        rangeVal_->setText(QString("%1 dB").arg(range_->value()));
        mapDayVal_->setText(QString("%1 %").arg(mapDay_->value()));
        mapNightVal_->setText(QString("%1 %").arg(mapNight_->value()));
        const bool isMap = bg_->currentIndex() >= 2;
        mapDay_->setEnabled(isMap);
        mapNight_->setEnabled(isMap);
    };
    connect(ref_,   &QSlider::valueChanged, this, [this, updateLabels] {
        updateLabels();
        emitChanged();
    });
    connect(range_, &QSlider::valueChanged, this, [this, updateLabels] {
        updateLabels();
        emitChanged();
    });
    connect(mapDay_, &QSlider::valueChanged, this, [this, updateLabels] {
        updateLabels();
        emitChanged();
    });
    connect(mapNight_, &QSlider::valueChanged, this, [this, updateLabels] {
        updateLabels();
        emitChanged();
    });
    connect(avg_,   &QComboBox::currentIndexChanged, this, &DisplayPanel::emitChanged);
    connect(speed_, &QComboBox::currentIndexChanged, this, &DisplayPanel::emitChanged);
    connect(pal_,   &QComboBox::currentIndexChanged, this, &DisplayPanel::emitChanged);
    connect(bg_, &QComboBox::currentIndexChanged, this, [this, updateLabels](int idx) {
        // Last entry = "Map: Custom…" — ask for the image right here. Cancel
        // with no previous custom pick bounces back to the prior backdrop.
        if (idx == bg_->count() - 1) {
            QSettings s;
            const QString cur = s.value("display/mapCustomPath").toString();
            const QString f = QFileDialog::getOpenFileName(
                this, "Choose a world map / backdrop image",
                cur.isEmpty() ? QDir::homePath() : cur,
                "Images (*.jpg *.jpeg *.png *.bmp)");
            if (!f.isEmpty()) {
                s.setValue("display/mapCustomPath", f);
            } else if (cur.isEmpty()) {
                const QSignalBlocker b(bg_);
                bg_->setCurrentIndex(prevBg_);
                updateLabels();
                return;
            }
        }
        prevBg_ = idx;
        updateLabels();
        emitChanged();
    });
    connect(fill_,  &QCheckBox::toggled, this, &DisplayPanel::emitChanged);
    connect(peak_,  &QCheckBox::toggled, this, &DisplayPanel::emitChanged);
    connect(grid_,  &QCheckBox::toggled, this, &DisplayPanel::emitChanged);
    connect(solar_, &QCheckBox::toggled, this, &DisplayPanel::emitChanged);
    connect(rose_,  &QCheckBox::toggled, this, &DisplayPanel::emitChanged);
    connect(plan_,  &QCheckBox::toggled, this, &DisplayPanel::emitChanged);
    connect(bigVfo_, &QCheckBox::toggled, this, &DisplayPanel::emitChanged);
    connect(clock_, &QCheckBox::toggled, this, &DisplayPanel::emitChanged);
    connect(wfTime_, &QCheckBox::toggled, this, &DisplayPanel::emitChanged);
    connect(cursor_, &QCheckBox::toggled, this, &DisplayPanel::emitChanged);
    connect(zap_,   &QCheckBox::toggled, this, &DisplayPanel::emitChanged);
    connect(trace_, &QComboBox::currentIndexChanged, this,
            &DisplayPanel::emitChanged);
    connect(call_,  &QCheckBox::toggled, this, &DisplayPanel::emitChanged);
    connect(callEdit_, &QLineEdit::textEdited, this, [this](const QString& t) {
        emit callsignChanged(t.trimmed().toUpper());
    });
    connect(gridEdit_, &QLineEdit::textEdited, this, [this](const QString& t) {
        emit gridChanged(t.trimmed().toUpper());
    });

    setSettings(DisplaySettings{});                // defaults until owner restores
}

DisplaySettings DisplayPanel::settings() const {
    DisplaySettings s;
    s.refDb     = static_cast<float>(ref_->value());
    s.rangeDb   = static_cast<float>(range_->value());
    s.avgFrames = avg_->currentData().toInt();
    s.wfSpeed   = speed_->currentData().toInt();
    s.palette   = pal_->currentIndex();
    s.fillTrace  = fill_->isChecked();
    s.peakHold   = peak_->isChecked();
    s.background = bg_->currentIndex();
    s.mapDay     = mapDay_->value();
    s.mapNight   = mapNight_->value();
    s.showGrid   = grid_->isChecked();
    s.showCall   = call_->isChecked();
    s.showSolar  = solar_->isChecked();
    s.showRose   = rose_->isChecked();
    s.showBandPlan = plan_->isChecked();
    s.traceColor = trace_->currentIndex();
    s.bigVfo     = bigVfo_->isChecked();
    s.showClock  = clock_->isChecked();
    s.showWfTime = wfTime_->isChecked();
    s.showCursor = cursor_->isChecked();
    s.cwZap      = zap_->isChecked();
    s.split      = split_;
    s.wfRefDb    = wfRef_;
    return s;
}

void DisplayPanel::setSettings(const DisplaySettings& s) {
    split_ = s.split;
    wfRef_ = s.wfRefDb;
    const QSignalBlocker b1(ref_), b2(range_), b3(avg_), b4(speed_), b5(pal_),
        b6(fill_), b7(peak_), b8(bg_), b9(grid_), b10(call_),
        b11(mapDay_), b12(mapNight_), b13(solar_), b14(rose_),
        b15(plan_), b16(bigVfo_), b17(trace_), b18(clock_), b19(zap_),
        b20(wfTime_), b21(cursor_);
    ref_->setValue(static_cast<int>(s.refDb));
    range_->setValue(static_cast<int>(s.rangeDb));
    const int ai = avg_->findData(s.avgFrames);
    avg_->setCurrentIndex(ai >= 0 ? ai : 2);
    const int si = speed_->findData(s.wfSpeed);
    speed_->setCurrentIndex(si >= 0 ? si : 1);
    pal_->setCurrentIndex(std::clamp(s.palette, 0, pal_->count() - 1));
    bg_->setCurrentIndex(std::clamp(s.background, 0, bg_->count() - 1));
    prevBg_ = bg_->currentIndex();
    mapDay_->setValue(std::clamp(s.mapDay, mapDay_->minimum(), mapDay_->maximum()));
    mapNight_->setValue(std::clamp(s.mapNight, mapNight_->minimum(),
                                   mapNight_->maximum()));
    fill_->setChecked(s.fillTrace);
    peak_->setChecked(s.peakHold);
    grid_->setChecked(s.showGrid);
    call_->setChecked(s.showCall);
    solar_->setChecked(s.showSolar);
    rose_->setChecked(s.showRose);
    plan_->setChecked(s.showBandPlan);
    bigVfo_->setChecked(s.bigVfo);
    clock_->setChecked(s.showClock);
    wfTime_->setChecked(s.showWfTime);
    cursor_->setChecked(s.showCursor);
    zap_->setChecked(s.cwZap);
    trace_->setCurrentIndex(std::clamp(s.traceColor, 0, trace_->count() - 1));
    refVal_->setText(QString("%1 dB").arg(ref_->value()));
    rangeVal_->setText(QString("%1 dB").arg(range_->value()));
    mapDayVal_->setText(QString("%1 %").arg(mapDay_->value()));
    mapNightVal_->setText(QString("%1 %").arg(mapNight_->value()));
    const bool isMap = bg_->currentIndex() >= 2;
    mapDay_->setEnabled(isMap);
    mapNight_->setEnabled(isMap);
}

void DisplayPanel::setCallsign(const QString& call) {
    const QSignalBlocker b(callEdit_);
    callEdit_->setText(call);
}

void DisplayPanel::setGrid(const QString& grid) {
    const QSignalBlocker b(gridEdit_);
    gridEdit_->setText(grid);
}

void DisplayPanel::emitChanged() {
    emit settingsChanged(settings());
}

} // namespace ttc
