// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QDialog>
#include <QVector>

#include "ui/PanadapterWidget.h"    // SpotLabel

class QLabel;
class QPushButton;
class QTableWidget;
class QTimer;

namespace ttc {

class LogbookIndex;
class RotorLink;

// The cluster feed as a table — HRD's DX-cluster window, kept: one row per
// spot with the country/band/mode needed-status columns (green check /
// amber ring / red cross), the call colored by the band map's worked-before
// ramp, and a live azimuth column. Click a bearing to turn the rotor;
// double-click a row to tune the radio there and feed the call to the LOG
// window, exactly like clicking the spot's label on the band map.
class SpotTableWindow : public QDialog {
    Q_OBJECT
public:
    SpotTableWindow(LogbookIndex* idx, RotorLink* rotor,
                    QWidget* parent = nullptr);

    void setSpots(const QVector<SpotLabel>& spots);   // from pushSpots

    // Contest mode: the rows arrive contest-classified (SpotLabel::
    // contest) and cross-band — THIS window is where "do I need to go
    // to another band for a mult" gets answered. Calls color by contest
    // value, the C column carries the verdict, NEED ONLY means "new
    // mults only", and the lifetime-logbook dots stand down.
    void setContestMode(bool on);

signals:
    // Double-clicked row: tune the radio and run the spot-click feed.
    void spotActivated(const QString& call, qint64 hz, QChar kind,
                       const QString& tag);

protected:
    void showEvent(QShowEvent* e) override;

private:
    void rebuild();
    QString modeGuess(const SpotLabel& l) const;

    LogbookIndex* idx_;
    RotorLink* rotor_;
    QVector<SpotLabel> spots_;
    QTableWidget* table_ = nullptr;
    QLabel* count_ = nullptr;
    QLabel* bands_ = nullptr;        // contest: per-band Qs/mults line
    QPushButton* fDx_ = nullptr;
    QPushButton* fPota_ = nullptr;
    QPushButton* fFt8_ = nullptr;
    QPushButton* fSkim_ = nullptr;
    QPushButton* fFdv_ = nullptr;
    QPushButton* fNeed_ = nullptr;
    QTimer* refresh_ = nullptr;
    bool dirty_ = false;
    bool contestMode_ = false;
};

} // namespace ttc
