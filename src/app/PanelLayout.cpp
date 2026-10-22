// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/PanelLayout.h"

#include <QBoxLayout>
#include <QGridLayout>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <algorithm>
#include <cstdlib>

#include "app/MainWindowInternal.h"
#include "app/SideBar.h"
#include "ui/containers/ContainerManager.h"
#include "ui/ControlPanel.h"
#include "ui/SMeterWidget.h"

namespace ttc {

namespace {
constexpr const char* kBg = "#0c1016";
// The box layout anywhere under `l` that directly holds `w`.
QBoxLayout* boxHolding(QLayout* l, QWidget* w) {
    if (!l) return nullptr;
    if (auto* b = qobject_cast<QBoxLayout*>(l); b && b->indexOf(w) >= 0) return b;
    for (int i = 0; i < l->count(); ++i)
        if (QLayout* sub = l->itemAt(i)->layout())
            if (QBoxLayout* hit = boxHolding(sub, w)) return hit;
    return nullptr;
}
}  // namespace

PanelLayout::PanelLayout(QWidget* window) : QObject(window), window_(window) {
    central_ = new QWidget(window);
    central_->setAutoFillBackground(true);
    QPalette pal = central_->palette();
    pal.setColor(QPalette::Window, QColor(kBg));
    central_->setPalette(pal);
    // Body row (popup column | pan | sidebar) over the TX bar, which runs
    // the window's full width: it is ~1600 px of fixed controls and under
    // the pan alone it pushed the window past the screen.
    outer_ = new QVBoxLayout(central_);
    outer_->setContentsMargins(0, 0, 0, 0);
    outer_->setSpacing(0);
    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    outer_->addLayout(row, 1);

    // Popup column, left of the pan.
    popCol_ = new QVBoxLayout;
    popCol_->setContentsMargins(6, 6, 6, 6);
    popCol_->setSpacing(0);
    row->addLayout(popCol_);

    center_ = new QVBoxLayout;
    center_->setContentsMargins(0, 0, 0, 0);
    center_->setSpacing(0);
    topBar_ = new QHBoxLayout;
    topBar_->setContentsMargins(10, 4, 10, 4);
    center_->addLayout(topBar_);
    row->addLayout(center_, 1);

    // The sidebar (app/SideBar, AetherSDR's AppletPanel): ONE fixed width,
    // a toggle grid on top, a single column of boxes in a scroll area.
    side_ = new SideBar(window, central_, "panel/", kSidebarWidth, kToggleCols);
    row->addWidget(side_->widget());

    // Tools: the second-deck buttons (LOG, CW, SKIM, CNTST, ROT) as a row.
    toolBody_ = new QWidget;
    toolRow_ = new QHBoxLayout(toolBody_);
    toolRow_->setContentsMargins(6, 6, 6, 6);
    toolRow_->setSpacing(0);
    // Transmit: the Tune split toggle (MainWindow builds it, Panel only).
    txBody_ = new QWidget;
    txLay_ = new QVBoxLayout(txBody_);
    txLay_->setContentsMargins(6, 6, 6, 6);
}

PanelLayout::~PanelLayout() = default;

QBoxLayout* PanelLayout::menuRow() { return popCol_; }
QBoxLayout* PanelLayout::toolRow() { return toolRow_; }

QWidget* PanelLayout::wrap(QLayout* l) {
    auto* w = new QWidget;
    l->setContentsMargins(6, 6, 6, 6);
    w->setLayout(l);
    return w;
}

ContainerWidget* PanelLayout::addBox(const QString& id, const QString& title,
                                     const QString& toggle, QWidget* body) {
    return side_->addBox(id, title, toggle, body);
}

void PanelLayout::placeMeter(QWidget* meter) {
    // The face grows with its box (docked: the sidebar width; floating:
    // whatever the window is stretched to).
    if (auto* sm = qobject_cast<SMeterWidget*>(meter)) sm->setScalable(true);
    for (auto* sm : meter->findChildren<SMeterWidget*>()) sm->setScalable(true);
    auto* l = new QHBoxLayout;
    l->addWidget(meter);
    addBox("meter", "METER", "METER", wrap(l));
}

void PanelLayout::placeZoom(QLayout* zoom) {
    topBar_->addLayout(zoom);
    topBar_->addStretch(1);
}

void PanelLayout::placeVfo(int vfo, QLayout* block) {
    QWidget* body = wrap(block);
    // The volume slider is a fixed 128 px (Classic's top strip); here it
    // takes what the column leaves.
    for (auto* sl : body->findChildren<QSlider*>()) {
        sl->setMinimumWidth(40);
        sl->setMaximumWidth(QWIDGETSIZE_MAX);
        sl->setSizePolicy(QSizePolicy::Expanding, sl->sizePolicy().verticalPolicy());
    }
    if (vfo == 0) addBox("vfoA", "VFO A", "VFO A", body);
    else          addBox("vfoB", "VFO B", "VFO B", body);
}

void PanelLayout::placeRouting(QWidget* routing) {
    // Classic spaces the antenna grid, transfers and VFO grid 14 px apart;
    // the column needs them closer.
    if (auto* rl = qobject_cast<QBoxLayout*>(routing->layout())) rl->setSpacing(6);
    auto* l = new QHBoxLayout;
    l->addStretch(1);
    l->addWidget(routing);
    l->addStretch(1);
    addBox("routing", "ROUTING", "ROUTING", wrap(l));
}

void PanelLayout::placeTransmit(QWidget* w) {
    txLay_->addWidget(w);
}

void PanelLayout::placeMain(QWidget* pan, QWidget* txBar, QWidget* controls) {
    center_->addWidget(pan, 1);
    panIndex_ = center_->indexOf(pan);
    outer_->addWidget(txBar);
    // One column: transmit and tools under the VFO boxes, then the receive
    // controls; BAND leaves the controls for the flyout.
    addBox("transmit", "TRANSMIT", "TX", txBody_);
    addBox("tools", "TOOLS", "TOOLS", toolBody_);
    buildBandFlyout(controls);
    // ControlPanel is a fixed 200 px in Classic; in the column it takes the
    // column's width.
    controls->setMinimumWidth(0);
    controls->setMaximumWidth(QWIDGETSIZE_MAX);
    addBox("rx", "RECEIVE", "RX", controls);
    // Rose, tuner card, solar block and AUDIO close the column.
    for (const PendingBox& b : sideBoxes_) addBox(b.id, b.title, b.toggle, b.w);
    side_->endColumn();
    popCol_->addStretch(1);
    toolRow_->addStretch(1);
}

void PanelLayout::buildBandFlyout(QWidget* controls) {
    auto* cp = qobject_cast<ControlPanel*>(controls);
    if (!cp) return;
    QWidget* band = cp->group(ControlPanel::Band);
    if (QBoxLayout* b = boxHolding(cp->layout(), band)) b->removeWidget(band);
    // A popup holding the band group as it is (buttons, stack readout);
    // the panel's own look comes along (the sheet's ControlPanel rule
    // simply doesn't match the popup).
    auto* pop = new QWidget(window_, Qt::Popup);
    pop->setAttribute(Qt::WA_StyledBackground, true);
    pop->setStyleSheet(cp->styleSheet() +
                       "QWidget#bandFlyout { background: #10151c;"
                       " border: 1px solid #2a3644; }");
    pop->setObjectName("bandFlyout");
    auto* l = new QVBoxLayout(pop);
    l->setContentsMargins(8, 0, 8, 8);
    band->setParent(pop);
    l->addWidget(band);
    band->show();
    pop->setFixedWidth(cp->width() > 0 ? cp->width() : 200);

    auto* btn = new QToolButton(central_);
    btn->setText("BAND");
    btn->setFocusPolicy(Qt::NoFocus);
    btn->setStyleSheet(kSideBtnStyle);
    btn->setToolTip("Band buttons (the radio's own band keys stay in sync)");
    popCol_->insertWidget(0, btn);
    popCol_->insertSpacing(1, 8);
    connect(btn, &QToolButton::clicked, pop, [btn, pop] {
        pop->adjustSize();
        pop->move(btn->mapToGlobal(QPoint(btn->width() + 4, 0)));
        pop->show();
    });
    connect(cp, &ControlPanel::bandSelected, pop, [pop] { pop->hide(); });
}

void PanelLayout::placeSideBox(const QString& id, const QString& title,
                               const QString& toggle, QWidget* w, bool tail) {
    sideBoxes_.push_back({id, title, toggle, w, tail});
}

void PanelLayout::setSideBoxVisible(const QString& id, bool on) {
    side_->boxes()->setContainerVisible(id, on);
}

QStringList PanelLayout::sideButtonNames() const { return side_->buttonNames(); }
void PanelLayout::setHiddenSideButtons(const QStringList& names) { side_->setHidden(names); }

void PanelLayout::placeUnderPan(QWidget* w) {
    center_->insertWidget(panIndex_ + 1, w);
}

void PanelLayout::finish() {
    // The tool buttons (LOG, CW, SKIM, CNTST, ROT…) arrive as Classic's
    // one row; in the column they wrap onto a grid sized by the box, like
    // the toggle grid, so another tool adds a row and never width.
    constexpr int kToolCols = 3;
    auto* grid = new QGridLayout;
    grid->setSpacing(4);
    int n = 0;
    while (QLayoutItem* it = toolRow_->takeAt(0)) {   // spacings/stretch go
        if (QWidget* w = it->widget()) {
            w->setSizePolicy(QSizePolicy::Ignored, w->sizePolicy().verticalPolicy());
            grid->addWidget(w, n / kToolCols, n % kToolCols);
            ++n;
        }
        delete it;
    }
    for (int c = 0; c < kToolCols; ++c) grid->setColumnStretch(c, 1);
    toolRow_->addLayout(grid, 1);

    side_->restore();
    // Selftest: measure just before main.cpp's quit timer fires.
    if (const char* st = std::getenv("TTC_SELFTEST"))
        QTimer::singleShot(std::max(500, std::atoi(st) * 1000 - 1500), this,
                           [this] { side_->checkWidth("panel"); });
}

} // namespace ttc
