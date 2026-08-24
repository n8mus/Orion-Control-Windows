// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QDialog>
#include <QWidget>

#include <functional>

class QCheckBox;
class QComboBox;
class QGridLayout;
class QGroupBox;
class QLabel;
class QPushButton;
class QSlider;
class QSpinBox;
class QTimer;
class QVBoxLayout;

namespace ttc {

class WinKeyer;

// Draws the letter R (dit dah dit) to scale with the current timing, over
// a ghost of the same letter at the keyer's factory settings. The K1EL
// manual uses R for exactly these figures (weighting p8, key comp p13,
// ratio p14), so the picture lines up with the document the numbers came
// from. Purely a visualiser — it never touches the keyer, which is the
// point: you can see what a change does without keying a transmitter.
class CwShapeWidget : public QWidget {
    Q_OBJECT
public:
    explicit CwShapeWidget(QWidget* parent = nullptr);
    void setTiming(int wpm, int weight, int keyCompMs, int ratio,
                   int letterspace);
    QSize sizeHint() const override { return {520, 92}; }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    int wpm_ = 25, weight_ = 50, keyComp_ = 0, ratio_ = 50, letterspace_ = 0;
};

// Full WinKeyer control box. Every control writes to the keyer live,
// through a 40 ms coalescing timer — the keyer is a 1200 baud link and a
// slider drag would otherwise flood it (same idiom as ControlPanel's
// gainTx_).
//
// The panel owns no truth: values live in QSettings under cw/wk/*, and
// WinKeyer::loadOwned() re-adopts them on every connect. A parameter the
// operator has never touched is never written, so a keyer set up by hand
// keeps its own timing until somebody deliberately changes it here.
class WinKeyerPanel : public QDialog {
    Q_OBJECT
public:
    WinKeyerPanel(WinKeyer* keyer, QWidget* parent = nullptr);

protected:
    // Wheel guard: only a focused control may be edited by the wheel.
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    void buildTimingGroup(QVBoxLayout* lay);
    void buildSendingGroup(QVBoxLayout* lay);
    void buildPttGroup(QVBoxLayout* lay);
    void buildPaddleGroup(QVBoxLayout* lay);
    void buildPresetRow(QVBoxLayout* lay);
    // One slider + spin + caption row wired to a live setter. Returns the
    // slider so the caller can keep a handle for presets.
    QSlider* addRow(QVBoxLayout* v, const QString& label,
                    const QString& hint, int lo, int hi, int cur,
                    const QString& settingKey,
                    std::function<void(int)> apply);
    void refreshPreview();
    void refreshPttTail();
    void applyPreset(const QString& name);
    void savePreset(const QString& name);
    void refreshPresetList();
    void restoreFactoryDefaults();
    void setManaged(const QString& settingKey, int value);

    WinKeyer* wk_ = nullptr;
    CwShapeWidget* shape_ = nullptr;
    QLabel*  fwLabel_ = nullptr;
    QLabel*  tailLabel_ = nullptr;
    QSlider* wpm_ = nullptr;
    QSlider* weight_ = nullptr;
    QSlider* keyComp_ = nullptr;
    QSlider* firstExt_ = nullptr;
    QSlider* ratio_ = nullptr;
    QSlider* letterspace_ = nullptr;
    QSlider* pttLead_ = nullptr;
    QSlider* pttTail_ = nullptr;
    QSlider* farns_ = nullptr;
    QSlider* switchpoint_ = nullptr;
    QComboBox* sidetone_ = nullptr;
    QGroupBox* paddleBox_ = nullptr;
    QCheckBox* manageMode_ = nullptr;
    QComboBox* keyMode_ = nullptr;
    QCheckBox* swap_ = nullptr;
    QCheckBox* autospace_ = nullptr;
    QCheckBox* ctSpace_ = nullptr;
    QComboBox* presets_ = nullptr;
};

} // namespace ttc
