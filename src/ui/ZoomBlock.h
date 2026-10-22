// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

class QHBoxLayout;
class QLabel;
class QSlider;
class QWidget;

namespace ttc {

// The panadapter zoom control: "ZOOM" label + slider (log scale, 0 = full
// span .. 100 = deepest zoom). Builds the widgets only — MainWindow wires the
// slider to the panadapter. Handed out as a layout, like VfoBlock, so Classic
// places it exactly as the old inline code did.
class ZoomBlock {
public:
    explicit ZoomBlock(QWidget* parent);
    QHBoxLayout* layout() const { return row_; }
    QSlider* slider() const { return slider_; }

private:
    QHBoxLayout* row_;
    QLabel* label_;
    QSlider* slider_;
};

} // namespace ttc
