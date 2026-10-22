// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QObject>
#include <QString>
#include <QVector>

#include "app/LayoutHost.h"

class QHBoxLayout;
class QVBoxLayout;

namespace ttc {

class ContainerWidget;
class SideBar;

// The opt-in "Panel" window (ui/layout=panel, docs/panel-layout-plan.md),
// AetherSDR/SmartSDR style: the panadapter in the middle with a popup
// column on its left (BAND flyout + the DISPLAY/SPOTS/AUDIO/SDR menus) and
// the TX bar along the window bottom; on the right a sidebar of ONE fixed
// width (kSidebarWidth, AetherSDR's AppletPanel idea: its contents are
// sized by it, never the other way round) holding a single column of
// boxes — meter, VFO A, routing (today's grid), VFO B, transmit (Tune
// split), tools, receive controls. Every box floats/docks/hides
// (ui/containers) and the arrangement is remembered under panel/.
// Same widgets as Classic, wired by MainWindow; only the placement differs.
class PanelLayout : public QObject, public LayoutHost {
    Q_OBJECT
public:
    // The sidebar's width, the operator's pick (2026-10-17; AetherSDR's is
    // 260). Fixed: showing, hiding, floating boxes or adding toggles never
    // moves it — the selftest checks (TTC_PANELCHECK=1).
    static constexpr int kSidebarWidth = 300;
    static constexpr int kToggleCols = 4;

    explicit PanelLayout(QWidget* window);
    ~PanelLayout() override;

    QWidget* buttonParent() override { return central_; }
    QBoxLayout* menuRow() override;
    QBoxLayout* toolRow() override;
    void placeMeter(QWidget* meter) override;
    void placeZoom(QLayout* zoom) override;
    void placeVfo(int vfo, QLayout* block) override;
    void placeRouting(QWidget* routing) override;
    void finishVfos(QLayout*, QLayout*) override {}
    void placeMain(QWidget* pan, QWidget* txBar, QWidget* controls) override;
    void placeSideBox(const QString& id, const QString& title,
                      const QString& toggle, QWidget* w, bool tail = false) override;
    void setSideBoxVisible(const QString& id, bool on) override;
    void placeUnderPan(QWidget* w) override;
    QStringList sideButtonNames() const override;
    void setHiddenSideButtons(const QStringList& names) override;
    QWidget* central() override { return central_; }

    bool isPanel() const override { return true; }
    void placeTransmit(QWidget* w) override;
    void finish() override;

private:
    struct PendingBox { QString id, title, toggle; QWidget* w; bool tail; };
    QVector<PendingBox> sideBoxes_;    // placeSideBox, joined in placeMain
    // A docked box in the sidebar column holding `body`, plus its toggle.
    ContainerWidget* addBox(const QString& id, const QString& title,
                            const QString& toggle, QWidget* body);
    QWidget* wrap(QLayout* l);
    void buildBandFlyout(QWidget* controls);

    QWidget* window_;
    QWidget* central_;
    QVBoxLayout* outer_;       // body row over the TX bar
    QVBoxLayout* popCol_;      // left of the pan: BAND + the menu buttons
    QHBoxLayout* topBar_;      // over the pan: zoom
    QVBoxLayout* center_;      // top bar, pan, under-pan slot, TX bar
    SideBar* side_;            // fixed kSidebarWidth (app/SideBar)
    QWidget* toolBody_;
    QHBoxLayout* toolRow_;
    QWidget* txBody_;
    QVBoxLayout* txLay_;
    int panIndex_ = -1;
};

} // namespace ttc
