// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QWidget>

class QLabel;
class QSlider;
class QTimer;
class QToolButton;

namespace ttc {

// The AUDIO sidebar box (was the AUDIO dropdown): the Orion's output routing
// matrix (volume + mute live under the VFO readouts in the top strip) —
//
//            A    B   A+B
//   PH L    [x]  [ ]  [ ]      <- one VFO per ear for split work
//   PH R    [ ]  [x]  [ ]
//   SPKR    [ ]  [ ]  [x]
//
// plus the TX audio monitor level (rarely touched, so it lives here rather
// than spending TX-bar space). show*() never emit.
class AudioPanel : public QWidget {
    Q_OBJECT
public:
    explicit AudioPanel(QWidget* parent = nullptr);

    void showRouting(char left, char right, char speaker);
    void showMonitor(int pct);

signals:
    void routingEdited(char left, char right, char speaker);
    void monitorChanged(int pct);                  // coalesced while sliding

private:
    void emitRouting();
    QToolButton* cell(const QString& tip);

    QToolButton* route_[3][3] = {}; // [row L/R/SPKR][col A/B/A+B]
    QSlider* mon_ = nullptr;
    QLabel*  monVal_ = nullptr;
    QTimer*  monTx_ = nullptr;
    int      pendMon_ = -1;
    bool     updating_ = false;
};

} // namespace ttc
