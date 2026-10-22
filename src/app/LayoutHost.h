// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QStringList>

class QBoxLayout;
class QLayout;
class QWidget;

namespace ttc {

// Where the main window's pieces go. MainWindow builds the controls and
// wires them; a LayoutHost decides where they sit. ClassicLayout is today's
// window (the top strip grid, pan + TX bar, control sidebar); a Panel layout
// (docs/panel-layout-plan.md) will answer the same calls differently.
// MainWindow calls these in a fixed order, once, from its constructor.
class LayoutHost {
public:
    virtual ~LayoutHost() = default;

    // Parent for toolbar/menu buttons, and the two button rows: the menu
    // corner (DISPLAY/SPOTS/AUDIO/SDR) and the tools row (LOG, CW, SKIM…).
    virtual QWidget* buttonParent() = 0;
    virtual QBoxLayout* menuRow() = 0;
    virtual QBoxLayout* toolRow() = 0;

    virtual void placeMeter(QWidget* meter) = 0;
    virtual void placeZoom(QLayout* zoom) = 0;
    virtual void placeVfo(int vfo, QLayout* block) = 0;   // 0 = A, 1 = B
    virtual void placeRouting(QWidget* routing) = 0;
    // After both VFOs are placed (Classic balances the two columns).
    virtual void finishVfos(QLayout* a, QLayout* b) = 0;
    // The panadapter, the TX bar under it, the receive-control sidebar.
    virtual void placeMain(QWidget* pan, QWidget* txBar, QWidget* controls) = 0;
    // A sidebar box (id for its saved state, box title, its toggle's
    // label). Called before placeMain; the boxes join the sidebar there —
    // Original: at the column's head, or after RECEIVE with `tail`.
    virtual void placeSideBox(const QString& id, const QString& title,
                              const QString& toggle, QWidget* w,
                              bool tail = false) = 0;
    // Show/hide a box whose toggle was left out of the grid (SOLAR: the
    // DISPLAY panel's tick owns it). Only after finish() — restoring the
    // saved arrangement would overwrite it.
    virtual void setSideBoxVisible(const QString& id, bool on) = 0;
    // A panel docked under the panadapter (the contest deck).
    virtual void placeUnderPan(QWidget* w) = 0;

    // The finished window body, for QMainWindow::setCentralWidget.
    virtual QWidget* central() = 0;

    // Panel-only pieces. MainWindow builds them only when isPanel(), so
    // Classic never gains a stray widget.
    virtual bool isPanel() const { return false; }
    virtual void placeTransmit(QWidget* /*w*/) {}   // the Tune split toggle
    // The sidebar's button grid by name, and the ones the operator keeps
    // out of it (DISPLAY panel picker).
    virtual QStringList sideButtonNames() const = 0;
    virtual void setHiddenSideButtons(const QStringList& names) = 0;
    // End of the MainWindow constructor: everything is placed (Panel
    // restores its saved box arrangement here).
    virtual void finish() {}
};

} // namespace ttc
