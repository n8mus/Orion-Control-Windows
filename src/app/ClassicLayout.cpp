// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/ClassicLayout.h"

#include <QAbstractButton>
#include <QBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QPalette>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>
#include <cstdlib>

#include "app/MainWindowInternal.h"
#include "app/SideBar.h"
#include "ui/containers/ContainerManager.h"
#include "ui/ControlPanel.h"

namespace ttc {

ClassicLayout::ClassicLayout(QWidget* window) : QObject(window), window_(window) {
    // S-meter above the panadapter, the sidebar on the right, the TX bar
    // under both (~1600 px of fixed controls: under the pan alone, beside a
    // 300 px sidebar, it would push the window past the 1850 budget).
    central_ = new QWidget(window);
    outer_ = new QVBoxLayout(central_);
    outer_->setContentsMargins(0, 0, 0, 0);
    outer_->setSpacing(0);
    lay_ = new QHBoxLayout;
    lay_->setContentsMargins(0, 0, 0, 0);
    lay_->setSpacing(0);
    outer_->addLayout(lay_, 1);
    left_ = new QVBoxLayout;           // the contest deck slots in here
    left_->setContentsMargins(0, 0, 0, 0);
    left_->setSpacing(0);
    // The meter is compact and fixed-size; park it left in a dark strip so the
    // rest of the row doesn't show the default window color.
    topStrip_ = new QWidget(window);
    topStrip_->setAutoFillBackground(true);
    QPalette strip = topStrip_->palette();
    strip.setColor(QPalette::Window, QColor(12, 16, 22));
    topStrip_->setPalette(strip);
    // Two decks on a GRID so deck 1 items align exactly with deck 2
    // columns: row 0 = S-meter, zoom (in VFO B's grid column, so it sits
    // precisely over B's fields); row 1 = the VFO columns with the routing
    // matrices between them.
    // Column plan: 0 smeter/stretch | 1 colA | 2 gap | 3 routing | 4 gap
    //              | 5 colB + zoom | 6 stretch
    // The menu corner that used to sit right of VFO B lives in the sidebar
    // now; the cluster packs together and the spare width splits evenly
    // left of the meter and right of VFO B (the old rule centring the
    // routing over the pan kept 280 px empty on the right for it).
    topGrid_ = new QGridLayout(topStrip_);
    topGrid_->setContentsMargins(10, 4, 10, 4);
    topGrid_->setHorizontalSpacing(0);
    topGrid_->setVerticalSpacing(3);
    topGrid_->setColumnStretch(0, 1);
    topGrid_->setColumnStretch(6, 1);
    topGrid_->setColumnMinimumWidth(0, 280);
    topGrid_->setColumnMinimumWidth(2, 24);
    topGrid_->setColumnMinimumWidth(4, 24);
    // Columns 1/5 get equal minimums computed AFTER the VFO columns are
    // built (B is naturally wider — VIEW button), see finishVfos().

    // The menu buttons (DISPLAY/SPOTS/AUDIO/SDR) and the tools (LOG, CW,
    // SKIM…) collect in these two rows while MainWindow builds them;
    // finish() moves them into the sidebar's button grid in that order.
    topLay_ = new QHBoxLayout;
    topLay2_ = new QHBoxLayout;
}

void ClassicLayout::placeMeter(QWidget* meter) {
    // Column 0, spanning both decks: just the meter, pushed right so it sits
    // beside VFO A. (The clock moved to the bottom of the radio panel — its
    // fixed width was pushing the window past narrow screens.)
    auto* col0 = new QHBoxLayout;
    col0->setContentsMargins(0, 0, 0, 0);
    col0->addStretch(1);
    col0->addWidget(meter, 0, Qt::AlignVCenter);
    col0->addSpacing(14);
    topGrid_->addLayout(col0, 0, 0, 2, 1);
}

void ClassicLayout::placeZoom(QLayout* zoom) {
    // Level with the S-meter, spanning VFO B's column: label at B's left
    // edge, slider running to the column's right edge (the VIEW button's).
    topGrid_->addLayout(zoom, 0, 5);
}

void ClassicLayout::placeVfo(int vfo, QLayout* block) {
    if (vfo == 0) topGrid_->addLayout(block, 1, 1, Qt::AlignRight);   // hug the center block
    else          topGrid_->addLayout(block, 1, 5, Qt::AlignLeft);
}

void ClassicLayout::placeRouting(QWidget* routing) {
    topGrid_->addWidget(routing, 1, 3);
}

void ClassicLayout::finishVfos(QLayout* a, QLayout* b) {
    // Equalize the VFO columns so the transfer buttons sit at the middle
    // of the VFO cluster.
    const int sym = std::max(a->sizeHint().width(), b->sizeHint().width());
    topGrid_->setColumnMinimumWidth(1, sym);
    topGrid_->setColumnMinimumWidth(5, sym);
}

namespace {
// A ControlPanel group lifted into a box of its own: the panel's style
// sheet comes along (its rules are type selectors, all but the
// ControlPanel background match here) and the box title replaces the
// group's own heading.
QWidget* liftGroup(ControlPanel* cp, ControlPanel::Group g) {
    QWidget* grp = cp->group(g);
    for (auto* l : cp->findChildren<QBoxLayout*>())
        if (l->indexOf(grp) >= 0) { l->removeWidget(grp); break; }
    // The heading stays (it names the group — the layout guard's script
    // log reads it) but takes no room and draws nothing.
    if (auto* gb = qobject_cast<QGroupBox*>(grp))
        gb->setStyleSheet("QGroupBox { margin-top: 0; }"
                          " QGroupBox::title { color: transparent; }");
    auto* body = new QWidget;
    body->setStyleSheet(cp->styleSheet());
    auto* l = new QVBoxLayout(body);
    l->setContentsMargins(8, 6, 8, 6);
    l->addWidget(grp);
    grp->show();
    return body;
}
}  // namespace

void ClassicLayout::placeMain(QWidget* pan, QWidget* txBar, QWidget* controls) {
    left_->addWidget(topStrip_);
    left_->addWidget(pan, 1);
    lay_->addLayout(left_, 1);
    outer_->addWidget(txBar);
    // Built only now, after the strip, pan and TX bar joined the window:
    // child order is what findChildren() walks, and the layout guard's
    // script addresses sliders by that order (the VFO volumes, then the
    // TX bar's, as before the sidebar).
    side_ = new SideBar(window_, central_, "classic/side/", kSidebarWidth, kGridCols);
    side_->addCollapseBar();
    lay_->addWidget(side_->widget());
    // Rose, tuner card and solar block head the column (they left the
    // waterfall's corners, where the contest logger covered them).
    for (const PendingBox& b : sideBoxes_)
        if (!b.tail) side_->addBox(b.id, b.title, b.toggle, b.w);
    // The receive controls as boxes: BAND, MODE, then the rest (AGC down to
    // the manual notch) as RECEIVE; the clock pinned under the column.
    if (auto* cp = qobject_cast<ControlPanel*>(controls)) {
        side_->addBox("band", "BAND", "BAND", liftGroup(cp, ControlPanel::Band));
        side_->addBox("mode", "MODE", "MODE", liftGroup(cp, ControlPanel::ModeGroup));
        QWidget* clock = cp->clockWidget();
        if (auto* l = qobject_cast<QBoxLayout*>(cp->layout())) l->removeWidget(clock);
        auto* foot = new QWidget;
        foot->setStyleSheet("QWidget { background: #0c1016; }");
        auto* fl = new QVBoxLayout(foot);
        fl->setContentsMargins(0, 4, 0, 6);
        fl->addWidget(clock);
        side_->setFooter(foot);
    }
    // ControlPanel is a fixed 200 px on its own; in the column it takes the
    // column's width.
    controls->setMinimumWidth(0);
    controls->setMaximumWidth(QWIDGETSIZE_MAX);
    side_->addBox("rx", "RECEIVE", "RX", controls);
    for (const PendingBox& b : sideBoxes_)          // AUDIO: after the receiver
        if (b.tail) side_->addBox(b.id, b.title, b.toggle, b.w);
    side_->endColumn();
}

void ClassicLayout::placeSideBox(const QString& id, const QString& title,
                                 const QString& toggle, QWidget* w, bool tail) {
    sideBoxes_.push_back({id, title, toggle, w, tail});
}

void ClassicLayout::setSideBoxVisible(const QString& id, bool on) {
    side_->boxes()->setContainerVisible(id, on);
}

QStringList ClassicLayout::sideButtonNames() const { return side_->buttonNames(); }
void ClassicLayout::setHiddenSideButtons(const QStringList& names) { side_->setHidden(names); }

void ClassicLayout::placeUnderPan(QWidget* w) {
    left_->insertWidget(2, w);                     // under the pan
}

void ClassicLayout::finish() {
    // The menu and tool buttons, in their old left-to-right order, into the
    // sidebar's grid ahead of the box toggles (sized by the grid: another
    // button adds a row, never width).
    int at = 0;
    for (QBoxLayout* row : {static_cast<QBoxLayout*>(topLay_),
                            static_cast<QBoxLayout*>(topLay2_)}) {
        while (QLayoutItem* it = row->takeAt(0)) {   // spacings go
            if (QWidget* w = it->widget()) {
                QString name = w->objectName();
                if (auto* b = qobject_cast<QAbstractButton*>(w))
                    name = b->text().remove(QStringLiteral("▾")).trimmed();
                side_->addButton(name, w, at++);
            }
            delete it;
        }
    }
    // The operator's grid (2026-10-22), four across:
    //   DISPLAY SPOTS SDR BAND / CW LOG SKIM CNTST /
    //   ROSE ROT TUNER METER / MODE RX AUDIO EQ.
    side_->setOrder({"DISPLAY", "SPOTS", "SDR", "BAND",
                     "CW", "LOG", "SKIM", "CNTST",
                     "ROSE", "ROT", "TUNER", "METER",
                     "MODE", "RX", "AUDIO", "EQ"});
    side_->restore();
    if (const char* st = std::getenv("TTC_SELFTEST"))
        QTimer::singleShot(std::max(500, std::atoi(st) * 1000 - 1500), this,
                           [this] { side_->checkWidth("classic"); });
}

} // namespace ttc
