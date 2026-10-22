// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QWidget>
#include "ui/PanadapterWidget.h"   // DisplaySettings

class QSlider;
class QLabel;
class QComboBox;
class QCheckBox;
class QLineEdit;
class QGridLayout;

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
    // The sidebar's buttons as checkboxes (ticked = shown); no emit. The
    // checkboxes live on their own panel (sidebarPicker) for a submenu.
    void setSidebarButtons(const QStringList& names, const QStringList& hidden);
    QWidget* sidebarPicker() const { return sideHost_; }

signals:
    void settingsChanged(const DisplaySettings& s);
    void callsignChanged(const QString& call);     // watermark text edited
    void gridChanged(const QString& grid);         // station grid square edited
    void sidebarHiddenChanged(const QStringList& hidden);   // picker ticked

private:
    void emitChanged();
    void reloadShips();            // fleet-folder scan -> SHIP gallery combo
    static QString shipDir();      // ~/Pictures/starships

    QSlider*   ref_    = nullptr;
    QLabel*    refVal_ = nullptr;
    QSlider*   range_  = nullptr;
    QLabel*    rangeVal_ = nullptr;
    QComboBox* avg_    = nullptr;
    QComboBox* speed_  = nullptr;
    QComboBox* pal_    = nullptr;
    QComboBox* bg_     = nullptr;
    QComboBox* ship_   = nullptr;  // fleet gallery (live when bg = Ship)
    QSlider*   mapDay_   = nullptr;   // map day-side brightness (maps only)
    QLabel*    mapDayVal_ = nullptr;
    QSlider*   mapNight_ = nullptr;   // map night-side brightness
    QLabel*    mapNightVal_ = nullptr;
    int        prevBg_ = 0;           // restore target if Custom… is canceled
    QWidget*     sideHost_ = nullptr; // sidebar button picker (own panel)
    QGridLayout* sideGrid_ = nullptr;
    QCheckBox* fill_   = nullptr;
    QCheckBox* peak_   = nullptr;
    QCheckBox* grid_   = nullptr;
    QCheckBox* solar_  = nullptr;
    QCheckBox* voacap_ = nullptr;  // VOACAP contours on map backdrops
    QCheckBox* plan_   = nullptr;
    QCheckBox* priv_   = nullptr;
    QCheckBox* bigVfo_ = nullptr;
    QCheckBox* clock_  = nullptr;
    QCheckBox* wfTime_ = nullptr;
    QCheckBox* cursor_ = nullptr;
    QCheckBox* zap_    = nullptr;
    QComboBox* trace_  = nullptr;
    QComboBox* style_  = nullptr;  // trace style: classic / heat map
    QComboBox* width_  = nullptr;  // trace line width
    QComboBox* smooth_ = nullptr;  // FFT Smooth
    QComboBox* decomb_ = nullptr;  // FFT De-comb
    QSlider*   dssFloor_ = nullptr;   // 3D Floor / Gain / Span (AetherSDR's)
    QLabel*    dssFloorVal_ = nullptr;
    QSlider*   dssGain_ = nullptr;
    QLabel*    dssGainVal_ = nullptr;
    QSlider*   dssSpan_ = nullptr;
    QLabel*    dssSpanVal_ = nullptr;
    QCheckBox* dssShadow_ = nullptr;  // 3D passband shading
    QCheckBox* call_   = nullptr;
    QLineEdit* callEdit_ = nullptr;
    QLineEdit* gridEdit_ = nullptr;
    float split_ = 0.42f;   // no UI control (dragged in the panadapter); pass through
    float wfRef_ = -55.0f;  // waterfall ref — dragged on the waterfall edge; pass through
};

} // namespace ttc
