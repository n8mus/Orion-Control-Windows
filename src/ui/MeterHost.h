// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QElapsedTimer>
#include <QHash>
#include <QWidget>

#include "radio/TxMeter.h"

class QStackedWidget;
class QToolButton;

namespace AetherSDR {
class SMeterWidget;
class CrossNeedleMeterApplet;
class LpMeterApplet;
}

namespace ttc {

class RadioController;
class SMeterWidget;

// The Panel layout's meter box: a face bar (CONSOLE / ARC / NEEDLE /
// LP-100A) over one face at a time — our own S-meter widget (all its faces,
// click to cycle as always) and AetherSDR's arc S-meter, cross-needle
// power/SWR meter and LP-100A panel (src/ui/aether, their code unchanged).
// Their faces listen to the radio's S-meter / TX meter and to the external
// wattmeter directly; the choice is saved under Options::faceKey.
// Panel: all four faces, sized by its box. The Original (Classic) view keeps
// our own meter plain in the top deck (no host, no face bar — the fixed
// 266x(arc height) slot ate panadapter) and offers AetherSDR's faces in its
// METER sidebar box: a host with `console = false`, their three faces at the
// box's size, floatable like any other pane.
class MeterHost : public QWidget {
    Q_OBJECT
public:
    struct Options {
        bool needle = true;                        // offer the cross-needle face
        bool console = true;                       // offer OUR meter as a face
                                                   // (false: `own` is left where
                                                   // it is — a second host in the
                                                   // same window can't adopt it)
        QString faceKey = "panel/meterFace";       // QSettings key of the choice
    };
    MeterHost(SMeterWidget* own, RadioController* radio, QWidget* parent = nullptr)
        : MeterHost(own, radio, Options(), parent) {}
    MeterHost(SMeterWidget* own, RadioController* radio, const Options& opt,
              QWidget* parent = nullptr);
    // The external wattmeter (LP-100A / PowerMaster), once it exists.
    void attachTxMeter(TxMeter* m);

private:
    void showFace(const QString& id);
    void feedRx(double dbS9);
    void feedRadioTx(double fwd, double ref, double swr);
    void feedExt(const TxMeter::Reading& r);
    void setTx(bool on);
    bool extFresh() const { return extSeen_.isValid() && extSeen_.elapsed() < 600; }

    QStackedWidget* stack_;
    QString faceKey_;
    QHash<QString, QWidget*> faces_;
    QHash<QString, QToolButton*> buttons_;
    AetherSDR::SMeterWidget* arc_;
    AetherSDR::CrossNeedleMeterApplet* needle_ = nullptr;   // null without the face
    AetherSDR::LpMeterApplet* lp_;
    QElapsedTimer extSeen_;          // last external-meter carrier
    bool tx_ = false;
};

} // namespace ttc
