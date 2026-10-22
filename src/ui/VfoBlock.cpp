// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/VfoBlock.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>

#include "ui/FrequencyDisplay.h"

namespace ttc {

VfoBlock::VfoBlock(FrequencyDisplay* disp, int vfo, QWidget* parent)
    : disp_(disp), parent_(parent) {
    auto* col = new QVBoxLayout;
    col_ = col;
    col->setSpacing(3);
    col->addWidget(disp_);
    // Volume + mute right below the readout (KE9NS-style rounded buttons,
    // matching the sidebar controls).
    row_ = new QHBoxLayout;
    row_->setSpacing(6);
    vol_ = new QSlider(Qt::Horizontal, parent);
    vol_->setRange(0, 100);
    vol_->setFixedWidth(128);
    vol_->setFocusPolicy(Qt::NoFocus);
    vol_->setStyleSheet(
        "QSlider::groove:horizontal { height: 5px; background: #2a3644; border-radius: 2px; }"
        "QSlider::handle:horizontal { width: 14px; margin: -5px 0; border-radius: 7px;"
        " background: #6aa5d8; }");
    volLbl_ = new QLabel("--", parent);
    volLbl_->setFixedWidth(22);
    volLbl_->setStyleSheet("color: #8fa3b8; font-size: 11px; font-weight: bold;");
    mute_ = new QToolButton(parent);
    mute_->setText("MUTE");
    mute_->setCheckable(true);
    mute_->setFocusPolicy(Qt::NoFocus);
    mute_->setFixedHeight(19);
    mute_->setStyleSheet(
        "QToolButton { background: #1c2430; border: 1px solid #2a3644;"
        " border-radius: 3px; color: #8fa3b8; font-size: 10px; font-weight: bold;"
        " padding: 0 8px; }"
        "QToolButton:hover { border-color: #4a5a6e; }"
        "QToolButton:checked { background: #8a2727; border-color: #e05d5d;"
        " color: #ffecec; }");
    // LOCK: MainWindow sends the Orion's *AL/*BL (freezes the front-panel
    // knob) and refuses its own tune gestures on that VFO — the radio still
    // accepts CAT tuning while locked (live-verified), so both sides have
    // to hold the line.
    lock_ = new QToolButton(parent);
    lock_->setText("LOCK");
    lock_->setCheckable(true);
    lock_->setFocusPolicy(Qt::NoFocus);
    lock_->setFixedHeight(19);
    lock_->setToolTip(QString("Lock VFO %1: front-panel knob and all "
                              "console tuning (filter edges stay live)")
                          .arg(vfo == 0 ? 'A' : 'B'));
    lock_->setStyleSheet(
        "QToolButton { background: #1c2430; border: 1px solid #2a3644;"
        " border-radius: 3px; color: #8fa3b8; font-size: 10px; font-weight: bold;"
        " padding: 0 8px; }"
        "QToolButton:hover { border-color: #4a5a6e; }"
        "QToolButton:checked { background: #8a6a27; border-color: #e0b45d;"
        " color: #fff6e0; }");
    row_->addSpacing(4);
    row_->addWidget(vol_);
    row_->addWidget(volLbl_);
    row_->addWidget(mute_);
    row_->addWidget(lock_);
    row_->addStretch(1);
    col->addLayout(row_);
}

void VfoBlock::addRowButton(QToolButton* b) {
    b->setParent(parent_);
    row_->addWidget(b);
}

} // namespace ttc
