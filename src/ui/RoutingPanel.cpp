// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/RoutingPanel.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QToolButton>

namespace ttc {

namespace {
QLabel* mini(const QString& text, QWidget* parent) {
    // 10px bold to match the rest of the top strip (vol labels, MUTE/LOCK).
    auto* l = new QLabel(text, parent);
    l->setStyleSheet("color: #8fa3b8; font-size: 10px; font-weight: bold;");
    l->setAlignment(Qt::AlignCenter);
    return l;
}
} // namespace

QToolButton* RoutingPanel::cell(const char* accent, const QString& tip) {
    auto* b = new QToolButton(this);
    b->setCheckable(true);
    b->setFixedSize(36, 21);
    b->setFocusPolicy(Qt::NoFocus);
    b->setProperty("accent", accent);
    b->setToolTip(tip);
    return b;
}

RoutingPanel::RoutingPanel(QWidget* parent) : QWidget(parent) {
    // Color by FUNCTION: TX red, RX green, SUB blue; both antenna columns
    // share the blue. The panadapter's split markers will reuse these.
    setStyleSheet(
        "QToolButton { background: #1c2430; border: 1px solid #2a3644;"
        " border-radius: 2px; }"
        "QToolButton:hover { border-color: #4a5a6e; }"
        "QToolButton[accent=\"tx\"]:checked { background: #8a2727; border-color: #e05d5d; }"
        "QToolButton[accent=\"rx\"]:checked { background: #1f7a45; border-color: #3ecf7a; }"
        "QToolButton[accent=\"sub\"]:checked { background: #2f6d9e; border-color: #5db2f0; }"
        "QToolButton[accent=\"ant\"]:checked { background: #2f6d9e; border-color: #5db2f0; }"
        "QToolButton[accent=\"xfer\"] { color: #c8d4e0; font-size: 10px; font-weight: bold;"
        " padding: 0 6px; }"
        "QToolButton[accent=\"xfer\"]:pressed { background: #2a3644; }");

    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(14);

    // --- antenna routing: vertical port columns, like the radio ------------
    auto* ag = new QGridLayout;
    ag->setSpacing(3);
    ag->addWidget(mini("ANT", this), 0, 0);
    antHdr_[0] = mini("MAIN", this);
    antHdr_[1] = mini("SUB", this);
    ag->addWidget(antHdr_[0], 0, 1);
    ag->addWidget(antHdr_[1], 0, 2);
    static const char* portName[3] = {"1", "2", "RX"};
    static const char* antTip[2][3] = {
        {"main RX + TX on ANT 1", "main RX + TX on ANT 2",
         "main RX on the aux RX antenna (TX stays on ANT 1/2)"},
        {"sub RX on ANT 1", "sub RX on ANT 2", "sub RX on the aux RX antenna"},
    };
    for (int p = 0; p < 3; ++p)                      // port rows down the side
        ag->addWidget(mini(portName[p], this), p + 1, 0);
    for (int r = 0; r < 2; ++r) {                    // r: 0=MAIN column, 1=SUB
        for (int p = 0; p < 3; ++p) {
            ant_[r][p] = cell("ant", antTip[r][p]);
            ag->addWidget(ant_[r][p], p + 1, r + 1);
            connect(ant_[r][p], &QToolButton::clicked, this, [this, r, p] {
                for (int k = 0; k < 3; ++k)          // one port per receiver
                    ant_[r][k]->setChecked(k == p);
                emitAnt();
            });
        }
    }
    lay->addLayout(ag);

    // --- dial transfers ---------------------------------------------------
    auto* xc = new QVBoxLayout;
    xc->setSpacing(3);
    static const struct { const char* text; const char* tip; } xfers[] = {
        {"A▸B", "copy dial A to B"},
        {"A⇄B", "swap dials"},
        {"B▸A", "copy dial B to A"},
    };
    for (int i = 0; i < 3; ++i) {
        auto* b = new QToolButton(this);
        b->setProperty("accent", "xfer");
        b->setText(xfers[i].text);
        b->setToolTip(xfers[i].tip);
        b->setFixedHeight(21);
        b->setFocusPolicy(Qt::NoFocus);
        xc->addWidget(b);
        if (i == 0) connect(b, &QToolButton::clicked, this, &RoutingPanel::copyABRequested);
        if (i == 1) connect(b, &QToolButton::clicked, this, &RoutingPanel::swapABRequested);
        if (i == 2) connect(b, &QToolButton::clicked, this, &RoutingPanel::copyBARequested);
    }
    lay->addLayout(xc);

    // --- VFO assignment: A and B columns, functions down the side ----------
    auto* vg = new QGridLayout;
    vg->setSpacing(3);
    vg->addWidget(mini("VFO", this), 0, 0);
    vg->addWidget(mini("A", this), 0, 1);
    vg->addWidget(mini("B", this), 0, 2);
    static const char* fnName[3] = {"TX", "RX", "SUB"};
    static const char* vfoTip[2][3] = {
        {"transmit on VFO A", "main receiver on VFO A", "sub receiver on VFO A"},
        {"transmit on VFO B (split with RX on A)", "main receiver on VFO B",
         "sub receiver on VFO B"},
    };
    for (int f = 0; f < 3; ++f)                      // function rows down the side
        vg->addWidget(mini(fnName[f], this), f + 1, 0);
    for (int r = 0; r < 2; ++r) {                    // r: 0=A column, 1=B column
        for (int f = 0; f < 3; ++f) {
            static const char* fnAccent[3] = {"tx", "rx", "sub"};
            vfo_[r][f] = cell(fnAccent[f], vfoTip[r][f]);
            vg->addWidget(vfo_[r][f], f + 1, r + 1);
            // One VFO per function, managed by hand so showVfoAssignment can
            // also represent 'N' (none).
            connect(vfo_[r][f], &QToolButton::clicked, this, [this, r, f] {
                vfo_[1 - r][f]->setChecked(false);
                vfo_[r][f]->setChecked(true);        // no un-assign by clicking
                emitVfo();
            });
        }
    }
    lay->addLayout(vg);

    // Sane defaults until the radio's ?KV/?KA answers arrive.
    showVfoAssignment('A', 'B', 'A');
    showAntennaRouting('B', 'N', 'N');
}

void RoutingPanel::emitVfo() {
    if (updating_) return;
    const auto colOf = [this](int c) {
        return vfo_[0][c]->isChecked() ? 'A' : (vfo_[1][c]->isChecked() ? 'B' : 'N');
    };
    // ?KV order is [mainrx][subrx][tx]; columns here are TX, RX, SUB.
    emit vfoAssignmentEdited(colOf(1), colOf(2), colOf(0));
}

void RoutingPanel::setSplitOnly(bool on) {
    const bool en = !on;
    const QString tip = en ? QString()
                           : QStringLiteral("Not available on this radio");
    for (int r = 0; r < 2; ++r)
        for (int p = 0; p < 3; ++p) {              // whole antenna matrix
            ant_[r][p]->setEnabled(en);
            if (!en) ant_[r][p]->setToolTip(tip);
        }
    for (int r = 0; r < 2; ++r)                    // RX + SUB rows (TX = split stays)
        for (int f = 1; f < 3; ++f) {
            vfo_[r][f]->setEnabled(en);
            if (!en) vfo_[r][f]->setToolTip(tip);
        }
}

void RoutingPanel::emitAnt() {
    if (updating_) return;
    if (omni_) {
        // Columns are RX / TX; the *C1V states couple TX to RX except on the
        // aux jack: 0 = ANT1 both, 1 = ANT2 both, 2/3 = RX aux + TX ANT1/2.
        int rx = 0;
        for (int p = 0; p < 3; ++p)
            if (ant_[0][p]->isChecked()) rx = p;
        const int tx = ant_[1][1]->isChecked() ? 1 : 0;
        const int sel = rx == 2 ? 2 + tx : rx;
        showOmniAntenna(sel);            // normalize coupling in the display
        emit omniAntennaEdited(sel);
        return;
    }
    char out[3];
    for (int p = 0; p < 3; ++p) {
        const bool m = ant_[0][p]->isChecked(), s = ant_[1][p]->isChecked();
        out[p] = m && s ? 'B' : m ? 'M' : s ? 'S' : 'N';
    }
    emit antennaRoutingEdited(out[0], out[1], out[2]);
}

void RoutingPanel::setOmniMode(bool on) {
    omni_ = on;
    if (!on) return;                     // Orion form is the constructed default
    antHdr_[0]->setText("RX");
    antHdr_[1]->setText("TX");
    static const char* rxTip[3] = {
        "RX and TX on antenna 1", "RX and TX on antenna 2",
        "RX on the aux RX jack (pick the TX antenna in the TX column)"};
    static const char* txTip[2] = {"TX on antenna 1 (needs RX on the aux jack "
                                   "— the radio couples TX to RX otherwise)",
                                   "TX on antenna 2 (needs RX on the aux jack "
                                   "— the radio couples TX to RX otherwise)"};
    for (int p = 0; p < 3; ++p) {
        ant_[0][p]->setEnabled(true);
        ant_[0][p]->setToolTip(rxTip[p]);
    }
    ant_[1][0]->setToolTip(txTip[0]);
    ant_[1][1]->setToolTip(txTip[1]);
    ant_[1][2]->setEnabled(false);       // can't transmit on the aux RX jack
    ant_[1][2]->setToolTip("The aux jack is receive-only");
    showOmniAntenna(0);                  // sane display until ?C1V answers
}

void RoutingPanel::showOmniAntenna(int sel) {
    if (sel < 0 || sel > 3) return;
    updating_ = true;
    const int rx = sel <= 1 ? sel : 2;
    const int tx = (sel == 1 || sel == 3) ? 1 : 0;
    for (int p = 0; p < 3; ++p)
        ant_[0][p]->setChecked(p == rx);
    ant_[1][0]->setChecked(tx == 0);
    ant_[1][1]->setChecked(tx == 1);
    ant_[1][2]->setChecked(false);
    // TX pick is only free when RX sits on the aux jack.
    ant_[1][0]->setEnabled(rx == 2);
    ant_[1][1]->setEnabled(rx == 2);
    updating_ = false;
}

void RoutingPanel::showVfoAssignment(char mainRx, char subRx, char tx) {
    updating_ = true;
    const char byCol[3] = {tx, mainRx, subRx};      // grid order TX, RX, SUB
    for (int c = 0; c < 3; ++c) {
        vfo_[0][c]->setChecked(byCol[c] == 'A');
        vfo_[1][c]->setChecked(byCol[c] == 'B');
    }
    updating_ = false;
}

void RoutingPanel::showAntennaRouting(char ant1, char ant2, char rxAnt) {
    updating_ = true;
    const char port[3] = {ant1, ant2, rxAnt};
    for (int p = 0; p < 3; ++p) {
        ant_[0][p]->setChecked(port[p] == 'M' || port[p] == 'B');
        ant_[1][p]->setChecked(port[p] == 'S' || port[p] == 'B');
    }
    updating_ = false;
}

} // namespace ttc
