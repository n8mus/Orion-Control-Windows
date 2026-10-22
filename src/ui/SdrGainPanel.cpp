// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/SdrGainPanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QLabel>
#include <QSlider>

namespace ttc {

// Front-end gain, live-adjustable: too much gain = ADC overload warnings
// and a taller DC spike; too little buries weak signals. IF slider shows
// gain (right = hotter, mapped to the API's 59..20 dB reduction); LNA is
// the coarse attenuator (0 = max gain .. 8 = max attenuation).
SdrGainPanel::SdrGainPanel(QWidget* parent) : QWidget(parent) {
    setStyleSheet(
        "QWidget { background: #141b24; color: #c8d4e0; font-size: 13px; }"
        "QLabel[cap=\"1\"] { color: #8fa3b8; font-size: 12px; font-weight: bold; }"
        "QSlider::groove:horizontal { height: 6px; background: #2a3644; border-radius: 3px; }"
        "QSlider::handle:horizontal { width: 18px; margin: -7px 0; border-radius: 9px;"
        " background: #6aa5d8; }"
        "QComboBox { background: #1c2430; border: 1px solid #2a3644; border-radius: 3px;"
        " padding: 3px 8px; }"
        "QComboBox QAbstractItemView { background: #1c2430; color: #c8d4e0;"
        " selection-background-color: #2a3644; }");
    auto* gg = new QGridLayout(this);
    gg->setContentsMargins(14, 10, 14, 10);
    auto* ifCap = new QLabel("IF GAIN", this);
    ifCap->setProperty("cap", "1");
    gg->addWidget(ifCap, 0, 0);
    ifGain_ = new QSlider(Qt::Horizontal, this);
    ifGain_->setRange(20, 59);                       // stored as reduction dB
    ifGain_->setInvertedAppearance(true);            // right = more gain
    ifGain_->setFixedWidth(170);
    ifVal_ = new QLabel(this);
    ifVal_->setFixedWidth(48);
    gg->addWidget(ifGain_, 0, 1);
    gg->addWidget(ifVal_, 0, 2);
    auto* lnaCap = new QLabel("LNA", this);
    lnaCap->setProperty("cap", "1");
    gg->addWidget(lnaCap, 1, 0);
    lna_ = new QComboBox(this);
    for (int i = 0; i <= 8; ++i)
        lna_->addItem(QString("%1%2").arg(i).arg(i == 0 ? "  (max gain)"
                                              : i == 8 ? "  (max attn)" : ""));
    gg->addWidget(lna_, 1, 1, 1, 2);
    // Auto LNA: the RF AGC that keeps ADC peaks in the sweet spot found
    // by hand (LNA 6 = deaf, LNA 2 = clipping mutants, LNA 3 = three
    // skimmed calls a minute). Touching the LNA box takes manual control
    // back and switches this off.
    autoLna_ = new QCheckBox("Auto LNA (holds best skimmer level)",
                                     this);
    autoLna_->setToolTip(
        "Steps the LNA to keep wideband ADC peaks between -16 and -4 dBFS:\n"
        "up fast near clipping, down after 30 s of unused headroom.\n"
        "The S-meter compensates automatically. Manual LNA changes turn "
        "this off.");
    gg->addWidget(autoLna_, 2, 0, 1, 3);
}

} // namespace ttc
