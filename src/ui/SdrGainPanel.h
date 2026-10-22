// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QSlider;

namespace ttc {

// The RSP2's front-end gain: IF gain slider + value, LNA state, Auto LNA.
// Builds the widgets only; MainWindow wires them to the SDR (today inside
// the SDR ▾ menu; the Panel layout's SDR box can hold the same panel).
class SdrGainPanel : public QWidget {
public:
    explicit SdrGainPanel(QWidget* parent = nullptr);
    QSlider*   ifGain() const { return ifGain_; }
    QLabel*    ifValue() const { return ifVal_; }
    QComboBox* lna() const { return lna_; }
    QCheckBox* autoLna() const { return autoLna_; }

private:
    QSlider* ifGain_;
    QLabel* ifVal_;
    QComboBox* lna_;
    QCheckBox* autoLna_;
};

} // namespace ttc
