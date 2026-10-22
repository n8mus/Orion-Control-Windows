// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QWidget>

class QHBoxLayout;
class QVBoxLayout;
class QLabel;
class QSlider;
class QToolButton;

namespace ttc {

class FrequencyDisplay;

// One VFO's block: the frequency readout on top, its control row below —
// volume slider + value, MUTE, LOCK, then one extra button (CTUN on A, VIEW
// on B). Builds the widgets only; MainWindow wires them (the radio, the
// panadapter, settings). The block is handed out as a LAYOUT, not a widget:
// Classic drops it into the top strip's grid cell exactly as the old inline
// code did (a wrapper widget measured a few pixels different — the Classic
// guard caught it), and the Panel layout installs the same layout in a box.
class VfoBlock {
public:
    // Widgets are created as children of `parent`; `disp` is the block's
    // readout; `vfo` is 0 = A, 1 = B (it only shapes the LOCK tooltip).
    VfoBlock(FrequencyDisplay* disp, int vfo, QWidget* parent);

    QVBoxLayout* layout() const { return col_; }
    FrequencyDisplay* display() const { return disp_; }
    QSlider*     volume() const { return vol_; }
    QLabel*      volumeLabel() const { return volLbl_; }
    QToolButton* mute() const { return mute_; }
    QToolButton* lock() const { return lock_; }
    // Appends `b` to the end of the control row (CTUN / VIEW).
    void addRowButton(QToolButton* b);

private:
    FrequencyDisplay* disp_;
    QWidget* parent_;
    QVBoxLayout* col_;
    QSlider* vol_;
    QLabel* volLbl_;
    QToolButton* mute_;
    QToolButton* lock_;
    QHBoxLayout* row_;
};

} // namespace ttc
