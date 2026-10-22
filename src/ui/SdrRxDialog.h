// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QDialog>
#include <functional>

#include "audio/VoiceEq.h"
#include "sdr/SdrRx.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QSlider;

namespace ttc { class VoiceEqCurve; class SdrRxMeter; }

namespace ttc {

// SDR ▾ → Receive audio (WDSP)…: the SDR receiver's controls in a window
// that stays up while the operator listens and tries settings (a menu
// closed on every click — live-found). Owns no audio: every change goes
// out through changed(); MainWindow persists it and drives SdrRx.
class SdrRxDialog : public QDialog {
    Q_OBJECT
public:
    SdrRxDialog(bool on, const SdrRx::Settings& s, QWidget* parent = nullptr);
    void setOn(bool on);                           // reflect a refusal / stop
    void setVolumeMute(int db, bool mute);         // changed elsewhere (FreeDV window); no emit
    // Status line provider, polled ~4x a second while the window is open.
    void setStatusSource(std::function<QString()> f) { status_ = std::move(f); }
    // Meter feed, polled ~20x a second while the window is open.
    void setLevelSource(std::function<SdrRx::Levels()> f) { levels_ = std::move(f); }

signals:
    void changed(bool on, const ttc::SdrRx::Settings& s);

private:
    SdrRx::Settings collect() const;
    void emitChanged();

    QCheckBox* on_ = nullptr;
    QSlider*   vol_ = nullptr;
    QLabel*    volVal_ = nullptr;
    QComboBox* agc_ = nullptr;
    QComboBox* nb_ = nullptr;
    QComboBox* nr_ = nullptr;
    QCheckBox* snb_ = nullptr;
    QCheckBox* anf_ = nullptr;
    QCheckBox* rnn_ = nullptr;
    QCheckBox* eqOn_ = nullptr;
    QComboBox* eqPreset_ = nullptr;
    VoiceEqCurve* curve_ = nullptr;
    VoiceEqSettings eq_;                           // the curve's bands + on
    void loadEqPreset(int idx);                    // index into rxEqPresets
    QCheckBox* gateOn_ = nullptr;
    QDoubleSpinBox* gateThr_ = nullptr;
    QDoubleSpinBox* gateFloor_ = nullptr;
    QCheckBox* compOn_ = nullptr;
    QDoubleSpinBox* compThr_ = nullptr;
    QDoubleSpinBox* compRatio_ = nullptr;
    QDoubleSpinBox* compMakeup_ = nullptr;
    QCheckBox* mute_ = nullptr;
    QCheckBox* boost_ = nullptr;
    SdrRxMeter* meter_ = nullptr;
    QLabel*    info_ = nullptr;
    std::function<QString()> status_;
    std::function<SdrRx::Levels()> levels_;
    bool loading_ = false;
};

} // namespace ttc
