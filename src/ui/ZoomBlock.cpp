// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/ZoomBlock.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>

namespace ttc {

ZoomBlock::ZoomBlock(QWidget* parent) {
    label_ = new QLabel("ZOOM", parent);
    label_->setStyleSheet("color: #8fa3b8; font-size: 10px; font-weight: bold;");
    slider_ = new QSlider(Qt::Horizontal, parent);
    slider_->setRange(0, 100);
    slider_->setToolTip("Zoom (span) — Ctrl+wheel on the panadapter does the same");
    slider_->setStyleSheet(
        "QSlider::groove:horizontal { height: 4px; background: #2a3644; border-radius: 2px; }"
        "QSlider::handle:horizontal { width: 12px; margin: -5px 0; border-radius: 6px;"
        " background: #6aa5d8; }");
    row_ = new QHBoxLayout;
    row_->setContentsMargins(0, 0, 0, 0);
    row_->addWidget(label_);
    row_->addSpacing(4);
    row_->addWidget(slider_, 1);                    // fills the rest of its column
}

} // namespace ttc
