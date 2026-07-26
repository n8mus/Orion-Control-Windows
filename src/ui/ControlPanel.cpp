// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/ControlPanel.h"
#include "app/Bands.h"

#include <QButtonGroup>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <algorithm>
#include <QVBoxLayout>

namespace ttc {

namespace {

const struct { const char* label; Mode mode; } kModes[] = {
    {"USB", Mode::USB}, {"LSB", Mode::LSB},
    {"CWU", Mode::CWU}, {"CWL", Mode::CWL},
    {"AM",  Mode::AM},  {"FM",  Mode::FM},
};

const struct { const char* label; char agc; } kAgcs[] = {
    {"F", 'F'}, {"M", 'M'}, {"S", 'S'}, {"P", 'P'}, {"OFF", 'O'},
};

const char* kAttenLabels[] = {"0", "6", "12", "18"};

QPushButton* makeButton(const QString& text) {
    auto* b = new QPushButton(text);
    b->setCheckable(true);
    b->setFocusPolicy(Qt::NoFocus);
    b->setMinimumHeight(26);
    return b;
}

QGroupBox* makeGroup(const QString& title) {
    auto* g = new QGroupBox(title);
    g->setFlat(true);
    return g;
}

} // namespace

ControlPanel::ControlPanel(QWidget* parent) : QWidget(parent) {
    setFixedWidth(200);
    // Flex-ish dark theme; keep it on the panel so the app-wide palette is untouched.
    setStyleSheet(R"(
        ControlPanel { background: #10151c; }
        QGroupBox { color: #8fa3b8; border: none; margin-top: 14px;
                    font-size: 10px; font-weight: bold; }
        QGroupBox::title { subcontrol-origin: margin; left: 4px; }
        QPushButton { background: #1c2430; color: #c8d4e0; border: 1px solid #2a3644;
                      border-radius: 3px; font-size: 11px; }
        QPushButton:hover { background: #26303e; }
        QPushButton:checked { background: #2b5c8a; color: #ffffff;
                              border-color: #3f7cb4; }
        QLabel { color: #c8d4e0; font-size: 11px; }
        QSlider::groove:horizontal { height: 4px; background: #2a3644; border-radius: 2px; }
        QSlider::handle:horizontal { width: 12px; margin: -5px 0; border-radius: 6px;
                                     background: #6aa5d8; }
    )");
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, QColor(0x10, 0x15, 0x1c));
    setPalette(pal);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(8, 4, 8, 8);
    lay->setSpacing(4);

    // --- Band ------------------------------------------------------------
    auto* bandBox = makeGroup("BAND");
    auto* bandGrid = new QGridLayout(bandBox);
    bandGrid->setContentsMargins(0, 4, 0, 0);
    bandGrid->setSpacing(3);
    bandGroup_ = new QButtonGroup(this);
    bandGroup_->setExclusive(true);
    for (int i = 0; i < kBandCount; ++i) {
        auto* b = makeButton(kBands[i].label);
        b->setMinimumHeight(22);
        bandGroup_->addButton(b, i);
        bandGrid->addWidget(b, i / 4, i % 4);
    }
    connect(bandGroup_, &QButtonGroup::idClicked, this,
            [this](int id) { emit bandSelected(id); });
    // Band-stack indicator: which of the 4 registers (A-D) is active.
    // Pressing the active band's button again cycles the stack.
    stackLbl_ = new QLabel("STACK --");
    stackLbl_->setStyleSheet("color: #6f87a0; font-size: 10px;");
    bandGrid->addWidget(stackLbl_, (kBandCount + 3) / 4, 0, 1, 4,
                        Qt::AlignRight | Qt::AlignVCenter);
    lay->addWidget(bandBox);

    // --- Mode ------------------------------------------------------------
    auto* modeBox = makeGroup("MODE");
    auto* modeGrid = new QGridLayout(modeBox);
    modeGrid->setContentsMargins(0, 4, 0, 0);
    modeGrid->setSpacing(4);
    modeGroup_ = new QButtonGroup(this);
    modeGroup_->setExclusive(true);
    for (int i = 0; i < 6; ++i) {
        auto* b = makeButton(kModes[i].label);
        modeGroup_->addButton(b, i);
        modeGrid->addWidget(b, i / 2, i % 2);
    }
    connect(modeGroup_, &QButtonGroup::idClicked, this,
            [this](int id) { emit modeSelected(kModes[id].mode); });
    // Row 4, balancing the grid: SAM under AM — pseudo-sync-AM
    // (ECSS: USB with the carrier zero-beaten; the wheel drops to 10/1 Hz
    // steps while lit). DIG under FM — line-in vs mic audio switch.
    samBtn_ = makeButton("SAM");
    samBtn_->setToolTip("Synchronous-AM style reception: switches to a sideband —\n"
                        "zero-beat the AM carrier; wheel steps 10 Hz, 1 Hz with Shift.\n"
                        "Turning it off returns to your previous mode and filter.");
    modeGrid->addWidget(samBtn_, 3, 0);
    connect(samBtn_, &QPushButton::toggled, this, [this](bool on) {
        if (!samBtn_->signalsBlocked()) emit samToggled(on);
    });
    digBtn_ = makeButton("DIG");
    digBtn_->setToolTip("Digital audio: rear line-in on, mic + speech processor off\n"
                        "(your voice settings come back when turned off)");
    modeGrid->addWidget(digBtn_, 3, 1);
    connect(digBtn_, &QPushButton::toggled, this, [this](bool on) {
        if (!digBtn_->signalsBlocked()) emit digitalToggled(on);
    });
    lay->addWidget(modeBox);

    // --- AGC --------------------------------------------------------------
    auto* agcBox = makeGroup("AGC");
    auto* agcCol = new QVBoxLayout(agcBox);
    agcCol->setContentsMargins(0, 4, 0, 0);
    agcCol->setSpacing(4);
    auto* agcRow = new QHBoxLayout;
    agcRow->setSpacing(3);
    agcGroup_ = new QButtonGroup(this);
    agcGroup_->setExclusive(true);
    for (int i = 0; i < 5; ++i) {
        auto* b = makeButton(kAgcs[i].label);
        agcGroup_->addButton(b, i);
        agcRow->addWidget(b);
    }
    connect(agcGroup_, &QButtonGroup::idClicked, this,
            [this](int id) { emit agcSelected(kAgcs[id].agc); });
    agcCol->addLayout(agcRow);
    // Programmable-AGC group (*RMAT/*RMAH/*RMAD — acts with 'P' selected):
    // threshold in µV, hang in tenths of a second (0 = off), decay rate.
    // The radio quantizes; readbacks show its actual values.
    auto agcSubRow = [&](const char* cap, QSlider*& sl, QLabel*& val,
                         int lo, int hi, QTimer*& tx, int& pend, auto sig) {
        auto* row = new QHBoxLayout;
        row->setSpacing(4);
        auto* c = new QLabel(cap);
        c->setFixedWidth(26);
        row->addWidget(c);
        sl = new QSlider(Qt::Horizontal);
        sl->setRange(lo, hi);
        row->addWidget(sl, 1);
        val = new QLabel("--");
        val->setFixedWidth(46);
        val->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row->addWidget(val);
        agcCol->addLayout(row);
        tx = new QTimer(this);
        tx->setSingleShot(true);
        tx->setInterval(40);
        connect(tx, &QTimer::timeout, this, [this, &pend, sig] {
            if (pend >= 0) { (this->*sig)(pend); pend = -1; }
        });
        connect(sl, &QSlider::valueChanged, this, [this, &pend, tx](int v) {
            pend = v;
            if (!tx->isActive()) tx->start();
        });
    };
    agcSubRow("THR", agcThr_, agcThrVal_, 1, 150,
              agcThrTx_, pendAgcThr_, &ControlPanel::agcThresholdChanged);
    agcSubRow("HNG", agcHang_, agcHangVal_, 0, 100,      // tenths of a second
              agcHangTx_, pendAgcHang_, &ControlPanel::agcHangChanged);
    agcSubRow("DCY", agcDecay_, agcDecayVal_, 5, 500,
              agcDecayTx_, pendAgcDecay_, &ControlPanel::agcDecayChanged);
    connect(agcThr_, &QSlider::valueChanged, this,
            [this](int v) { agcThrVal_->setText(QString("%1 µV").arg(v)); });
    connect(agcHang_, &QSlider::valueChanged, this, [this](int v) {
        agcHangVal_->setText(v == 0 ? QString("off")
                                    : QString("%1 s").arg(v / 10.0, 0, 'f', 1));
    });
    connect(agcDecay_, &QSlider::valueChanged, this,
            [this](int v) { agcDecayVal_->setText(QString::number(v)); });
    lay->addWidget(agcBox);

    // --- Front end: attenuator + preamp --------------------------------------
    auto* attBox = makeGroup("ATTEN (dB) / PREAMP");
    auto* attRow = new QHBoxLayout(attBox);
    attRow->setContentsMargins(0, 4, 0, 0);
    attRow->setSpacing(3);
    attenGroup_ = new QButtonGroup(this);
    attenGroup_->setExclusive(true);
    for (int i = 0; i < 4; ++i) {
        auto* b = makeButton(kAttenLabels[i]);
        attenGroup_->addButton(b, i);
        attRow->addWidget(b);
    }
    connect(attenGroup_, &QButtonGroup::idClicked, this,
            [this](int id) { emit attenSelected(id); });
    attRow->addSpacing(6);
    preBtn_ = makeButton("PRE");
    attRow->addWidget(preBtn_);
    connect(preBtn_, &QPushButton::toggled, this,
            [this](bool on) { emit preampToggled(on); });
    lay->addWidget(attBox);

    // --- RF gain ------------------------------------------------------------
    auto* rfBox = makeGroup("RF GAIN");
    auto* rfRow = new QHBoxLayout(rfBox);
    rfRow->setContentsMargins(0, 4, 0, 0);
    rfGain_ = new QSlider(Qt::Horizontal);
    rfGain_->setRange(0, 100);
    rfGain_->setValue(100);
    rfGainVal_ = new QLabel("100");
    rfGainVal_->setFixedWidth(26);
    rfGainVal_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    rfRow->addWidget(rfGain_);
    rfRow->addWidget(rfGainVal_);
    lay->addWidget(rfBox);

    gainTx_ = new QTimer(this);
    gainTx_->setSingleShot(true);
    gainTx_->setInterval(40);
    connect(gainTx_, &QTimer::timeout, this, [this] {
        if (pendGain_ >= 0) { emit rfGainChanged(pendGain_); pendGain_ = -1; }
    });
    connect(rfGain_, &QSlider::valueChanged, this, [this](int v) {
        rfGainVal_->setText(QString::number(v));
        pendGain_ = v;
        if (!gainTx_->isActive()) gainTx_->start();
    });

    // --- PBT ------------------------------------------------------------------
    // Slider for deliberate passband-tuning shifts (Ctrl+drag in the passband
    // does the same by mouse), plus the quick re-center: "PBT 0" snaps back to
    // the detent (bandwidth kept). Also on double-click of a passband edge.
    auto* pbtBox = makeGroup("PBT");
    auto* pbtCol = new QVBoxLayout(pbtBox);
    pbtCol->setContentsMargins(0, 4, 0, 0);
    pbtCol->setSpacing(4);
    pbt_ = new QSlider(Qt::Horizontal);
    pbt_->setRange(-2000, 2000);
    pbt_->setSingleStep(10);
    pbt_->setPageStep(100);
    pbtCol->addWidget(pbt_);
    auto* pbtRow = new QHBoxLayout;
    auto* pbtBtn = new QPushButton("PBT 0");
    pbtBtn->setFocusPolicy(Qt::NoFocus);
    pbtBtn->setMinimumHeight(26);
    pbtVal_ = new QLabel("0 Hz");
    pbtVal_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    pbtRow->addWidget(pbtBtn);
    pbtRow->addWidget(pbtVal_, 1);
    pbtCol->addLayout(pbtRow);
    connect(pbtBtn, &QPushButton::clicked, this,
            [this] { emit pbtZeroRequested(); });
    pbtTx_ = new QTimer(this);
    pbtTx_->setSingleShot(true);
    pbtTx_->setInterval(40);
    connect(pbtTx_, &QTimer::timeout, this, [this] {
        if (pendPbt_ != -9999) { emit pbtChanged(pendPbt_); pendPbt_ = -9999; }
    });
    connect(pbt_, &QSlider::valueChanged, this, [this](int v) {
        pbtVal_->setText(QString("%1%2 Hz").arg(v > 0 ? "+" : "").arg(v));
        pendPbt_ = v;
        if (!pbtTx_->isActive()) pbtTx_->start();
    });
    lay->addWidget(pbtBox);

    // --- DSP: NR / NB / auto-notch -------------------------------------------
    // Queryable on v3 despite the docs (undocumented ?R.NR/NB/NA) — synced
    // from the radio at startup.
    lay->addWidget(makeDspRow("NR", nr_, nrVal_));
    lay->addWidget(makeDspRow("NB", nb_, nbVal_));

    // Hardware noise blanker (undocumented *RMNH, strict on/off).
    auto* hwBox = makeGroup("HARDWARE NB");
    auto* hwRow = new QHBoxLayout(hwBox);
    hwRow->setContentsMargins(0, 4, 0, 0);
    hwNbBtn_ = makeButton("ON");
    hwRow->addWidget(hwNbBtn_);
    connect(hwNbBtn_, &QPushButton::toggled, this,
            [this](bool on) { emit hwNbToggled(on); });
    lay->addWidget(hwBox);

    lay->addWidget(makeDspRow("AUTO NOTCH", an_, anVal_));

    // Manual notch / SAF: one DSP engine in the radio, two flavors — NOTCH
    // rejects the marked band, SAF (v3) peaks it instead (CW pileup picker).
    // Same center/width parameters drive both; engaging one steals the
    // engine from the other, exactly like the front-panel menu toggle.
    // Placement is on the panadapter (drag the marker; wheel = width).
    auto* notchBox = makeGroup("MANUAL NOTCH / SAF");
    auto* notchRow = new QHBoxLayout(notchBox);
    notchRow->setContentsMargins(0, 4, 0, 0);
    notchBtn_ = makeButton("NOTCH");
    notchBtn_->setToolTip("Reject the marked band");
    safBtn_ = makeButton("SAF");
    safBtn_->setToolTip("Spot Audio Filter: peak the marked band instead of "
                        "rejecting it (same marker, same width)");
    notchVal_ = new QLabel("--");
    notchVal_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    notchRow->addWidget(notchBtn_);
    notchRow->addWidget(safBtn_);
    notchRow->addWidget(notchVal_, 1);
    connect(notchBtn_, &QPushButton::toggled, this,
            [this](bool on) { emit notchToggled(on); });
    connect(safBtn_, &QPushButton::toggled, this,
            [this](bool on) { emit safToggled(on); });
    lay->addWidget(notchBox);
    connect(nr_, &QSlider::valueChanged, this, [this](int v) {
        nrVal_->setText(v ? QString::number(v) : "off");
        emit nrChanged(v);
    });
    connect(nb_, &QSlider::valueChanged, this, [this](int v) {
        nbVal_->setText(v ? QString::number(v) : "off");
        emit nbChanged(v);
    });
    connect(an_, &QSlider::valueChanged, this, [this](int v) {
        anVal_->setText(v ? QString::number(v) : "off");
        emit autoNotchChanged(v);
    });

    lay->addStretch(1);

    // Clock parked at the panel's very bottom (moved out of the top strip —
    // it was costing horizontal width the meter/VFO row needs). MainWindow's
    // ticker feeds it; the DISPLAY "Clock" checkbox shows/hides it.
    clockUtc_ = new QLabel(this);
    clockLoc_ = new QLabel(this);
    clockUtc_->setStyleSheet("color: #ffd23c; font-size: 13px; font-weight: bold;");
    clockLoc_->setStyleSheet("color: #8fa3b8; font-size: 12px; font-weight: bold;");
    clockUtc_->setAlignment(Qt::AlignHCenter);
    clockLoc_->setAlignment(Qt::AlignHCenter);
    lay->addWidget(clockUtc_);
    lay->addWidget(clockLoc_);
}

void ControlPanel::showClock(const QString& utc, const QString& local) {
    clockUtc_->setText(utc);
    clockLoc_->setText(local);
}

void ControlPanel::setClockVisible(bool on) {
    clockUtc_->setVisible(on);
    clockLoc_->setVisible(on);
}

QWidget* ControlPanel::makeDspRow(const QString& name, QSlider*& slider, QLabel*& value) {
    auto* box = makeGroup(name);
    auto* row = new QHBoxLayout(box);
    row->setContentsMargins(0, 4, 0, 0);
    slider = new QSlider(Qt::Horizontal);
    slider->setRange(0, 9);
    slider->setValue(0);
    value = new QLabel("off");
    value->setFixedWidth(26);
    value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    row->addWidget(slider);
    row->addWidget(value);
    return box;
}

void ControlPanel::showBand(int bandIdx) {
    QSignalBlocker block(bandGroup_);
    if (bandIdx < 0 || bandIdx >= kBandCount) {
        // Out of every ham band: uncheck whatever is lit.
        if (auto* b = bandGroup_->checkedButton()) {
            bandGroup_->setExclusive(false);
            b->setChecked(false);
            bandGroup_->setExclusive(true);
        }
        return;
    }
    if (auto* b = bandGroup_->button(bandIdx)) b->setChecked(true);
}

void ControlPanel::showBandStack(int regIdx) {
    if (regIdx < 0 || regIdx >= kStackCount) stackLbl_->setText("STACK --");
    else stackLbl_->setText(QString("STACK %1").arg(kStackNames[regIdx]));
}

void ControlPanel::showBandStackText(const QString& text) {
    stackLbl_->setText(text);                 // channelized bands (60 m CH1..CH5)
}

void ControlPanel::showMode(Mode m) {
    for (int i = 0; i < 6; ++i)
        if (kModes[i].mode == m) {
            QSignalBlocker block(modeGroup_);
            if (auto* b = modeGroup_->button(i)) b->setChecked(true);
            return;
        }
}

void ControlPanel::showAgc(char agc) {
    for (int i = 0; i < 5; ++i)
        if (kAgcs[i].agc == agc) {
            QSignalBlocker block(agcGroup_);
            if (auto* b = agcGroup_->button(i)) b->setChecked(true);
            return;
        }
}

void ControlPanel::showAtten(int step) {
    if (step < 0 || step > 3) return;
    QSignalBlocker block(attenGroup_);
    if (auto* b = attenGroup_->button(step)) b->setChecked(true);
}

void ControlPanel::showPreamp(bool on) {
    QSignalBlocker block(preBtn_);
    preBtn_->setChecked(on);
}

void ControlPanel::showRfGain(int gain) {
    QSignalBlocker block(rfGain_);
    rfGain_->setValue(gain);
    rfGainVal_->setText(QString::number(gain));
}

static void showDspLevel(QSlider* slider, QLabel* value, int level) {
    QSignalBlocker block(slider);
    slider->setValue(level);
    value->setText(level ? QString::number(level) : "off");
}

void ControlPanel::showNr(int level)        { showDspLevel(nr_, nrVal_, level); }
void ControlPanel::showNb(int level)        { showDspLevel(nb_, nbVal_, level); }
void ControlPanel::showAutoNotch(int level) { showDspLevel(an_, anVal_, level); }

void ControlPanel::showNotch(bool on, int centerHz, int widthHz) {
    QSignalBlocker block(notchBtn_);
    notchBtn_->setChecked(on);
    notchVal_->setText(QString("%1 Hz x %2").arg(centerHz).arg(widthHz));
}

void ControlPanel::showSaf(bool on) {
    QSignalBlocker block(safBtn_);
    safBtn_->setChecked(on);
}

void ControlPanel::showHwNb(bool on) {
    QSignalBlocker block(hwNbBtn_);
    hwNbBtn_->setChecked(on);
}

void ControlPanel::showAgcThreshold(double uv) {
    {
        QSignalBlocker b(agcThr_);
        agcThr_->setValue(std::clamp(static_cast<int>(std::lround(uv)), 1, 150));
    }
    agcThrVal_->setText(QString("%1 µV").arg(uv, 0, 'f', uv < 10.0 ? 1 : 0));
}

void ControlPanel::showSam(bool on) {
    QSignalBlocker b(samBtn_);
    samBtn_->setChecked(on);
}

void ControlPanel::setSamLabel(const QString& t) { samBtn_->setText(t); }

void ControlPanel::clearModeSelection() {
    QSignalBlocker block(modeGroup_);
    modeGroup_->setExclusive(false);      // exclusive groups refuse "none"
    for (auto* b : modeGroup_->buttons()) b->setChecked(false);
    modeGroup_->setExclusive(true);
}

void ControlPanel::showDigital(bool on) {
    QSignalBlocker b(digBtn_);
    digBtn_->setChecked(on);
}

void ControlPanel::showAgcHang(double sec) {
    {
        QSignalBlocker b(agcHang_);
        agcHang_->setValue(std::clamp(static_cast<int>(std::lround(sec * 10.0)), 0, 100));
    }
    agcHangVal_->setText(sec < 0.05 ? QString("off")
                                    : QString("%1 s").arg(sec, 0, 'f', 1));
}

void ControlPanel::showAgcDecay(int rate) {
    {
        QSignalBlocker b(agcDecay_);
        agcDecay_->setValue(std::clamp(rate, 5, 500));
    }
    agcDecayVal_->setText(QString::number(rate));
}

void ControlPanel::showPbt(int pbtHz) {
    {   // slider mirrors radio state without re-emitting; pegs beyond +/-2 kHz
        QSignalBlocker b(pbt_);
        pbt_->setValue(std::clamp(pbtHz, -2000, 2000));
    }
    pbtVal_->setText(QString("%1%2 Hz").arg(pbtHz > 0 ? "+" : "").arg(pbtHz));
    // Make an off-center passband visible at a glance.
    pbtVal_->setStyleSheet(pbtHz == 0 ? "color: #c8d4e0;" : "color: #f0b040;");
}

void ControlPanel::setReducedCatSet(bool reduced) {
    const bool on = !reduced;
    const QString tip = on ? QString()
                           : QStringLiteral("Not available on this radio");
    const std::initializer_list<QWidget*> ws = {
        hwNbBtn_, notchBtn_, safBtn_, notchVal_, preBtn_,
        agcThr_, agcThrVal_, agcHang_, agcHangVal_, agcDecay_, agcDecayVal_,
        agcGroup_ ? agcGroup_->button(3) : nullptr,   // 'P' — no programmable AGC
    };
    for (QWidget* w : ws) {
        if (!w) continue;
        w->setEnabled(on);
        if (!on) w->setToolTip(tip);
    }
    // DIG is the Orion/SignaLink line-in swap (drives the C1 gain group).
    // On the Omni VII it's a trap on BOTH transports: RADIO-mode serial
    // ignores the C1 commands entirely, and in remote it parks the mic gain
    // that TRIP voice depends on (live-found: 0 W FT8 until unlatched).
    // Omni digital lives on the radio's own front controls (serial) or
    // SDR > TX audio > Digital (remote).
    digBtn_->setEnabled(on);
    if (!on)
        digBtn_->setToolTip("Omni VII: choose the TX audio source on the "
                            "radio's front panel (serial), or SDR ▸ TX "
                            "audio ▸ Digital when remote");
}

} // namespace ttc
