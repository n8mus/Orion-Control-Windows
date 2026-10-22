// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QHBoxLayout>
#include <QObject>
#include <QString>
#include <QVector>

#include "app/LayoutHost.h"

class QGridLayout;
class QVBoxLayout;

namespace ttc {

class SideBar;

// The Original view: a dark top strip on a grid (S-meter | VFO A |
// routing | VFO B + zoom), the panadapter under it, a 300 px sidebar on the
// right (app/SideBar, the same machinery as Panel's: a button grid holding
// the menus and tools, then BAND / MODE / RECEIVE boxes that float, hide
// and rearrange, the clock pinned at its foot) and the TX bar along the
// whole window bottom (docs/original-sidebar-plan.md, 2026-10-18).
// The Classic layout guard (tests/classic_guard.sh) holds it to the pixel.
class ClassicLayout : public QObject, public LayoutHost {
    Q_OBJECT
public:
    // The sidebar: Panel's width (operator, 2026-10-18), 4 buttons across.
    static constexpr int kSidebarWidth = 300;
    static constexpr int kGridCols = 4;

    explicit ClassicLayout(QWidget* window);

    QWidget* buttonParent() override { return topStrip_; }
    QBoxLayout* menuRow() override { return topLay_; }
    QBoxLayout* toolRow() override { return topLay2_; }
    void placeMeter(QWidget* meter) override;
    void placeZoom(QLayout* zoom) override;
    void placeVfo(int vfo, QLayout* block) override;
    void placeRouting(QWidget* routing) override;
    void finishVfos(QLayout* a, QLayout* b) override;
    void placeMain(QWidget* pan, QWidget* txBar, QWidget* controls) override;
    void placeSideBox(const QString& id, const QString& title,
                      const QString& toggle, QWidget* w, bool tail = false) override;
    void setSideBoxVisible(const QString& id, bool on) override;
    void placeUnderPan(QWidget* w) override;
    QStringList sideButtonNames() const override;
    void setHiddenSideButtons(const QStringList& names) override;
    QWidget* central() override { return central_; }
    void finish() override;

private:
    struct PendingBox { QString id, title, toggle; QWidget* w; bool tail; };
    QVector<PendingBox> sideBoxes_;    // placeSideBox, joined in placeMain
    QWidget* window_;
    QWidget* central_;
    QVBoxLayout* outer_;
    QHBoxLayout* lay_;
    QVBoxLayout* left_;
    QWidget* topStrip_;
    QGridLayout* topGrid_;
    QHBoxLayout* topLay_;
    QHBoxLayout* topLay2_;
    SideBar* side_;
};

} // namespace ttc
