// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QWidget>
#include "ui/PanadapterWidget.h"   // DisplaySettings

class QSlider;
class QLabel;
class QComboBox;
class QCheckBox;
class QLineEdit;

namespace ttc {

// Flex-style display settings popup: reference level, dB range (contrast),
// spectrum averaging, waterfall speed, palette, fill and peak-hold. Lives in
// a QWidgetAction under the "DISPLAY" toolbar dropdown; every change emits
// settingsChanged live so the panadapter updates while you drag.
class DisplayPanel : public QWidget {
    Q_OBJECT
public:
    explicit DisplayPanel(QWidget* parent = nullptr);

    DisplaySettings settings() const;
    void setSettings(const DisplaySettings& s);    // no emit (QSignalBlocker)
    void setCallsign(const QString& call);         // no emit
    void setGrid(const QString& grid);             // no emit

signals:
    void settingsChanged(const DisplaySettings& s);
    void callsignChanged(const QString& call);     // watermark text edited
    void gridChanged(const QString& grid);         // station grid square edited

private:
    void emitChanged();

    QSlider*   ref_    = nullptr;
    QLabel*    refVal_ = nullptr;
    QSlider*   range_  = nullptr;
    QLabel*    rangeVal_ = nullptr;
    QComboBox* avg_    = nullptr;
    QComboBox* speed_  = nullptr;
    QComboBox* pal_    = nullptr;
    QComboBox* bg_     = nullptr;
    QSlider*   mapDay_   = nullptr;   // map day-side brightness (maps only)
    QLabel*    mapDayVal_ = nullptr;
    QSlider*   mapNight_ = nullptr;   // map night-side brightness
    QLabel*    mapNightVal_ = nullptr;
    int        prevBg_ = 0;           // restore target if Custom… is canceled
    QCheckBox* fill_   = nullptr;
    QCheckBox* peak_   = nullptr;
    QCheckBox* grid_   = nullptr;
    QCheckBox* solar_  = nullptr;
    QCheckBox* rose_   = nullptr;
    QCheckBox* plan_   = nullptr;
    QCheckBox* bigVfo_ = nullptr;
    QCheckBox* clock_  = nullptr;
    QCheckBox* wfTime_ = nullptr;
    QCheckBox* cursor_ = nullptr;
    QCheckBox* zap_    = nullptr;
    QComboBox* trace_  = nullptr;
    QCheckBox* call_   = nullptr;
    QLineEdit* callEdit_ = nullptr;
    QLineEdit* gridEdit_ = nullptr;
    float split_ = 0.42f;   // no UI control (dragged in the panadapter); pass through
    float wfRef_ = -55.0f;  // waterfall ref — dragged on the waterfall edge; pass through
};

} // namespace ttc
