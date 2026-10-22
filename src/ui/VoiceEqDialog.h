// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QDialog>
#include <QWidget>
#include <functional>
#include <limits>

#include "audio/VoiceEq.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QSpinBox;
class QTimer;

namespace ttc {

// The EQ response, drawn over the voice range with the Orion's own TX
// passband edge marked: what the PC shapes inside, what the radio cuts.
// Drag a band's handle to shape the curve — the interaction model of
// AetherSDR's ClientEqEditorCanvas: left/right = frequency, up/down =
// gain (peaks and shelves), Shift+drag or the wheel = Q; high/low-pass
// handles move in frequency only (their slope is the dB/oct box).
// Grabbing a band that is off turns it on. Every change goes out through
// onBandEdited so the band rows — and everything they drive — follow.
class VoiceEqCurve : public QWidget {
public:
    explicit VoiceEqCurve(QWidget* parent = nullptr);
    void setSettings(const VoiceEqSettings& s) { s_ = s; update(); }
    void setTarget(int idx) { target_ = idx; update(); }   // voiceEqTargets()
    // The shaded "radio cuts above here" edge; 0 hides it (the RX EQ).
    void setEdge(float hz, const QString& label) { edgeHz_ = hz; edgeLabel_ = label; update(); }
    QSize sizeHint() const override { return {560, 200}; }
    std::function<void(int band, const ClientEq::BandParams& p)> onBandEdited;
    std::function<void()> onDragFinished;

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;

private:
    QRectF plot() const;
    QPointF handlePos(int band) const;
    int hitTest(const QPointF& p) const;           // band index or -1
    float hzAt(double x) const;
    float dbAt(double y) const;
    void emitBand(int band, ClientEq::BandParams p);

    VoiceEqSettings s_;
    int target_ = 0;
    float edgeHz_ = 3900.f;                        // widest Orion TX filter
    QString edgeLabel_ = QStringLiteral("Orion TX edge →");
    int drag_ = -1, hover_ = -1;
    bool dragQ_ = false;                           // Shift held at grab
    QPointF dragStart_;
    float dragStartQ_ = 1.f;
};

// Live mic levels, pushed ~10x a second by MainWindow while LIVE runs.
struct LiveLevels {
    bool   live = false;
    float  inDb = -120.f, outDb = -120.f;          // block peaks, dBFS
    float  compDb = 0.f;                           // compressor GR (<= 0)
    bool   gateOn = false, gateOpen = true;
    double delayMs = 0;                            // queued toward the SignaLink
};

// The meter panel: mic in, level to the SignaLink with its pickup
// threshold (amber, once measured) and the limiter ceiling (red),
// compressor reduction, gate state, queue delay. Bars hold their peak
// and fall back ~20 dB/s so speech reads as a level, not a flicker.
class LiveMeter : public QWidget {
public:
    explicit LiveMeter(QWidget* parent = nullptr);
    void setLevels(const LiveLevels& l);
    void setMarks(float thresholdDb, float ceilingDb);   // NaN = not known
    QSize sizeHint() const override { return {560, 112}; }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    LiveLevels l_;
    float inShown_ = -120.f, outShown_ = -120.f, compShown_ = 0.f;
    float thrDb_ = std::numeric_limits<float>::quiet_NaN();
    float ceilDb_ = -1.f;
};

// Voice keyer EQ editor (AUDIO ▾ → Voice keyer EQ…). Non-modal and LIVE:
// every edit (preset, checkbox, number) re-renders the VK messages from
// their untouched takes ~0.4 s after the last change — no Apply step
// (live-found: with an Apply button, preset changes that were never
// applied went out unchanged and sounded like "the EQ does nothing").
// Owns no files; MainWindow does the rendering on applyRequested.
class VoiceEqDialog : public QDialog {
    Q_OBJECT
public:
    explicit VoiceEqDialog(const VoiceEqSettings& s, QWidget* parent = nullptr);
    void setStatus(const QString& text);
    void setLiveLevels(const LiveLevels& l);
    void setPickupThreshold(float db);             // NaN = not measured
    // Threshold finder progress: running + the tone level now, or done.
    void setCalibration(bool running, const QString& text);

signals:
    void applyRequested(const ttc::VoiceEqSettings& s);     // debounced: re-render
    void liveSettingsChanged(const ttc::VoiceEqSettings& s); // every edit: live mic
    // Apply first, then play this slot on the PC speakers (nothing keyed).
    void listenRequested(int slot, const ttc::VoiceEqSettings& s);
    void calibrateStartRequested();                // play the rising tone
    void calibrateMarkRequested();                 // "the light just came on"
    void calibrateCancelRequested();

private:
    VoiceEqSettings collect() const;
    void load(const VoiceEqSettings& s);
    void fillProfiles(const QString& select);      // built-ins + the operator's
    void setActiveProfile(const QString& key);     // remembered across opens
    void setModified(bool m);                      // "(modified)" on the name
    void saveAsPrompt();
    void refreshEnables();

    QWidget* buildDynamicsPage();
    // Dynamics controls (gate -> EQ -> de-esser -> compressor -> limiter).
    struct Dyn {
        QCheckBox *gateOn = nullptr, *deessOn = nullptr, *compOn = nullptr,
                  *limOn = nullptr, *limDc = nullptr;
        QDoubleSpinBox *gateThr = nullptr, *gateRatio = nullptr,
                       *gateFloor = nullptr, *gateHold = nullptr,
                       *gateRel = nullptr;
        QDoubleSpinBox *deHz = nullptr, *deThr = nullptr, *deAmt = nullptr;
        QDoubleSpinBox *cThr = nullptr, *cRatio = nullptr, *cAtt = nullptr,
                       *cRel = nullptr, *cKnee = nullptr, *cDrive = nullptr,
                       *cMakeup = nullptr;
        QSpinBox *cPhase = nullptr;
        QDoubleSpinBox *limCeil = nullptr;
    } dyn_;
    VoiceEqSettings base_;                         // carries fields not on screen
    struct Row {
        QCheckBox*      on    = nullptr;
        QSpinBox*       hz    = nullptr;
        QDoubleSpinBox* db    = nullptr;   // shelves + peaks
        QDoubleSpinBox* q     = nullptr;   // shelves + peaks
        QComboBox*      slope = nullptr;   // high-/low-pass
    };
    Row          rows_[VoiceEqSettings::kBands];
    QCheckBox*   enabled_ = nullptr;
    QComboBox*   preset_  = nullptr;
    QComboBox*   slot_    = nullptr;
    QComboBox*   target_  = nullptr;
    VoiceEqCurve* curve_  = nullptr;
    QLabel*      status_  = nullptr;
    LiveMeter*   meter_   = nullptr;
    QPushButton* calBtn_  = nullptr;
    QPushButton* calMark_ = nullptr;
    QLabel*      calText_ = nullptr;
    bool         calRunning_ = false;
    QTimer*      applyTimer_ = nullptr;           // debounces live apply
    bool         loading_ = false;
    bool         modified_ = false;                // edits not in the profile yet
    QPushButton* delBtn_ = nullptr;
};

} // namespace ttc
