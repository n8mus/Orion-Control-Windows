// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QWidget>

class QLabel;
class QToolButton;

namespace ttc {

// VFO/antenna routing strip for the top bar, laid out like the Orion's own
// front panel: vertical button columns, antennas on the left, VFO assignment
// on the right, dial transfers between them:
//
//   ANT MAIN SUB           VFO   A   B
//    1  [x]  [.]    A>B     TX  [x] [.]
//    2  [.]  [x]    A<>B    RX  [x] [.]
//    RX [.]  [.]    B>A    SUB  [.] [x]
//
// VFO grid: which VFO drives the transmitter / main RX / sub RX (one VFO per
// function; TX on B with RX on A is the radio's split). ANT grid: which port
// each receiver listens on (one port per receiver; TX follows main's port).
class RoutingPanel : public QWidget {
    Q_OBJECT
public:
    explicit RoutingPanel(QWidget* parent = nullptr);

    void showVfoAssignment(char mainRx, char subRx, char tx);   // no emit
    void showAntennaRouting(char ant1, char ant2, char rxAnt);  // no emit
    // Single-receiver radio (Omni 8): only the TX row (split) and the dial
    // transfers do anything — gray out the antenna matrix and sub-RX rows.
    void setSplitOnly(bool on);
    // Omni VII over Ethernet: the antenna matrix becomes the *C1V relay —
    // MAIN column = RX antenna (ANT1/ANT2/RX aux), SUB column = TX antenna
    // (ANT1/ANT2; the radio couples TX to RX unless RX is on the aux jack).
    // Call after setSplitOnly (it re-enables the matrix in its Omni form).
    void setOmniMode(bool on);
    void showOmniAntenna(int sel);       // reflect *C1V 0..3, no emit

signals:
    void vfoAssignmentEdited(char mainRx, char subRx, char tx);
    void antennaRoutingEdited(char ant1, char ant2, char rxAnt);
    void omniAntennaEdited(int sel);     // *C1V 0..3 (Omni mode only)
    void copyABRequested();     // dial A -> B
    void copyBARequested();     // dial B -> A
    void swapABRequested();     // dials exchange

private:
    void emitVfo();
    void emitAnt();
    QToolButton* cell(const char* accent, const QString& tip);

    QToolButton* vfo_[2][3] = {};   // [vfo A,B][function TX,RX,SUB]
    QToolButton* ant_[2][3] = {};   // [rx MAIN,SUB][port ANT1,ANT2,RXANT]
    QLabel* antHdr_[2] = {};        // MAIN/SUB headers (relabel RX/TX in Omni)
    bool updating_ = false;         // show*() must not re-emit
    bool omni_ = false;             // antenna matrix speaks *C1V
};

} // namespace ttc
