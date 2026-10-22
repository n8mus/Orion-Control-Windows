// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/SideBar.h"

#include <QApplication>
#include <QDragEnterEvent>
#include <QEvent>
#include <QGridLayout>
#include <QMimeData>
#include <QScrollArea>
#include <QSettings>
#include <QToolButton>
#include <QVBoxLayout>
#include <cstdio>

#include "app/MainWindowInternal.h"
#include "ui/containers/ContainerManager.h"
#include "ui/containers/ContainerWidget.h"

namespace ttc {

namespace {
constexpr const char* kBg = "#0c1016";
// The box column inside the sidebar's scroll area. AetherSDR's
// FlexContainer (AppletPanel.cpp): a width hint of 0, so the scroll area
// always sizes it to the viewport and no box can ask for more. It is also
// the drop target for a box's title-bar drag (their AppletDropArea idea).
class BoxDropArea : public QWidget {
public:
    BoxDropArea(SideBar* host, QWidget* parent) : QWidget(parent), host_(host) {
        setAcceptDrops(true);
    }
    QSize sizeHint() const override { return QSize(0, QWidget::sizeHint().height()); }
    // Theirs keeps the layout's minimum height; a resizable QScrollArea sizes
    // its content to max(viewport, minimum), so a column taller than the
    // window squashed every box to ITS minimum (the arc meter cut in half).
    // The full height as the minimum: the column scrolls, boxes keep shape.
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void dragEnterEvent(QDragEnterEvent* ev) override {
        if (ev->mimeData()->hasFormat(ContainerWidget::kDragMime)) ev->acceptProposedAction();
    }
    void dragMoveEvent(QDragMoveEvent* ev) override {
        if (!ev->mimeData()->hasFormat(ContainerWidget::kDragMime)) return;
        ev->acceptProposedAction();
        host_->showDropAt(ev->position().toPoint());
    }
    void dragLeaveEvent(QDragLeaveEvent*) override { host_->hideDrop(); }
    void dropEvent(QDropEvent* ev) override {
        host_->hideDrop();
        if (!ev->mimeData()->hasFormat(ContainerWidget::kDragMime)) return;
        const QString id = QString::fromUtf8(ev->mimeData()->data(ContainerWidget::kDragMime));
        if (host_->dropBox(id, ev->position().toPoint())) ev->acceptProposedAction();
    }

private:
    SideBar* host_;
};

}  // namespace

SideBar::SideBar(QWidget* window, QWidget* parent, const QString& prefix,
                 int width, int gridCols)
    : QObject(window), window_(window), prefix_(prefix), width_(width), cols_(gridCols) {
    QPalette pal = parent->palette();
    pal.setColor(QPalette::Window, QColor(kBg));
    side_ = new QWidget(parent);
    side_->setAutoFillBackground(true);
    side_->setPalette(pal);
    side_->setFixedWidth(width_);
    sideLay_ = new QVBoxLayout(side_);
    sideLay_->setContentsMargins(0, 0, 0, 0);
    sideLay_->setSpacing(0);
    gridHost_ = new QWidget(side_);
    grid_ = new QGridLayout(gridHost_);
    grid_->setContentsMargins(2, 3, 2, 3);
    grid_->setSpacing(2);
    for (int c = 0; c < cols_; ++c) grid_->setColumnStretch(c, 1);
    sideLay_->addWidget(gridHost_);
    scroll_ = new QScrollArea(side_);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);   // width never jumps
    scroll_->setWidgetResizable(true);
    scroll_->setStyleSheet(
        "QScrollArea { background: transparent; }"
        "QScrollBar:vertical { background: #0c1016; width: 12px; }"
        "QScrollBar::handle:vertical { background: #1c2430; border-radius: 4px; min-height: 20px; }"
        "QScrollBar::handle:vertical:hover { background: #4a6880; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }");
    stack_ = new BoxDropArea(this, nullptr);
    stack_->setAutoFillBackground(true);
    stack_->setPalette(pal);
    boxCol_ = new QVBoxLayout(stack_);
    boxCol_->setContentsMargins(0, 0, 0, 0);
    boxCol_->setSpacing(2);
    scroll_->setWidget(stack_);
    sideLay_->addWidget(scroll_, 1);
    dropLine_ = new QWidget(stack_);          // their 2 px accent line
    dropLine_->setStyleSheet("background: #5db2f0;");
    dropLine_->hide();

    boxes_ = new ContainerManager(window, prefix_, this);

    // Leaving: freeze the box arrangement before Qt closes the floating
    // windows (a close would otherwise read as "dock it"). Two ways out:
    // the main window's own close, and app.quit() — Qt 6 sends the
    // application a Quit event and THEN closes every window in no fixed
    // order (the selftest docked its floating box 4 runs in 5).
    window->installEventFilter(this);
    qApp->installEventFilter(this);
}

SideBar::~SideBar() = default;

bool SideBar::eventFilter(QObject* o, QEvent* ev) {
    if ((ev->type() == QEvent::Close && o == window_)
        || (ev->type() == QEvent::Quit && o == qApp))
        boxes_->prepareShutdown();
    return QObject::eventFilter(o, ev);
}

ContainerWidget* SideBar::addBox(const QString& id, const QString& title,
                                 const QString& toggle, QWidget* body) {
    ContainerWidget* c = boxes_->create(id, title, body);
    c->setAttribute(Qt::WA_StyledBackground, true);
    c->setStyleSheet(QString("#%1 { background: #121921;"
                             " border: 1px solid #22303e; }").arg(id));
    boxCol_->addWidget(c);
    if (toggle.isEmpty()) return c;              // visibility owned elsewhere
    auto* b = addToggle(toggle);
    cells_.last().boxId = id;
    b->setToolTip(QString("Show/hide the %1 box").arg(title));
    connect(b, &QToolButton::toggled, boxes_,
            [this, id](bool on) { boxes_->setContainerVisible(id, on); });
    connect(c, &ContainerWidget::visibilityChanged, b, [b](bool on) {
        const QSignalBlocker bl(b);
        b->setChecked(on);
    });
    return c;
}

QToolButton* SideBar::addToggle(const QString& text) {
    // AetherSDR's bar buttons: sized BY the grid (column stretch 1 each,
    // fixed 20 px height), never the other way round, so another button
    // adds a row and never width. Ignored rather than their Expanding so
    // even a long label can't raise the column minimum.
    auto* b = new QToolButton(gridHost_);
    b->setText(text);
    b->setCheckable(true);
    b->setChecked(true);
    b->setFocusPolicy(Qt::NoFocus);
    b->setStyleSheet(kSideBtnStyle);
    addButton(text, b);
    return b;
}

void SideBar::addButton(const QString& name, QWidget* w, int at) {
    w->setParent(gridHost_);
    w->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    w->setFixedHeight(20);
    if (at < 0 || at > cells_.size()) at = cells_.size();
    cells_.insert(at, {name, w, QString()});
    relayoutGrid();
}

void SideBar::setOrder(const QStringList& names) {
    QVector<Cell> out;
    for (const QString& n : names)
        for (int i = 0; i < cells_.size(); ++i)
            if (cells_[i].name == n) { out.push_back(cells_.takeAt(i)); break; }
    out += cells_;
    cells_ = out;
    relayoutGrid();
}

void SideBar::relayoutGrid() {
    for (const Cell& c : cells_) grid_->removeWidget(c.w);
    int n = 0;
    for (const Cell& c : cells_) {
        if (hidden_.contains(c.name)) {            // unticked in DISPLAY
            c.w->hide();
            continue;
        }
        grid_->addWidget(c.w, n / cols_, n % cols_);
        c.w->show();
        ++n;
    }
}

QStringList SideBar::buttonNames() const {
    QStringList out;
    for (const Cell& c : cells_) out << c.name;
    return out;
}

void SideBar::setHidden(const QStringList& names) {
    const QSet<QString> now(names.begin(), names.end());
    for (const Cell& c : cells_)
        if (!c.boxId.isEmpty() && now.contains(c.name) && !hidden_.contains(c.name))
            boxes_->setContainerVisible(c.boxId, false);
    hidden_ = now;
    relayoutGrid();
}

void SideBar::addCollapseBar() {
    auto* bar = new QToolButton(side_);
    bar->setFocusPolicy(Qt::NoFocus);
    bar->setStyleSheet(QString(kSideBtnStyle)
                       + "QToolButton { text-align: left; padding-left: 6px; }");
    bar->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    bar->setFixedHeight(18);
    bar->setToolTip("Show or fold away the sidebar buttons");
    const QString key = prefix_ + "buttonsOpen";
    const auto apply = [this, bar](bool open) {
        gridHost_->setVisible(open);
        bar->setText(open ? "▲  BUTTONS" : "▼  BUTTONS");
    };
    apply(QSettings().value(key, true).toBool());
    connect(bar, &QToolButton::clicked, this, [this, key, apply] {
        const bool open = !gridHost_->isVisible();
        apply(open);
        QSettings().setValue(key, open);
    });
    auto* wrapLay = new QVBoxLayout;
    wrapLay->setContentsMargins(2, 3, 2, 0);
    wrapLay->addWidget(bar);
    sideLay_->insertLayout(0, wrapLay);
}

void SideBar::setFooter(QWidget* w) {
    sideLay_->addWidget(w);
}

void SideBar::endColumn() {
    // Their stretch factor 1: spare height collects at the bottom instead of
    // opening gaps under boxes whose bodies expand vertically.
    boxCol_->addStretch(1);
}

void SideBar::restore() {
    restoreOrder();
    boxes_->restoreState();
}

void SideBar::checkWidth(const char* tag) {
    // The box column's slot: what every box gets, and must not exceed.
    const auto settle = [] {
        for (int i = 0; i < 3; ++i) QApplication::processEvents();
    };
    const auto measure = [this] {
        return QString("%1/%2/%3").arg(side_->width()).arg(stack_->width())
            .arg(window_->minimumSizeHint().width());
    };
    settle();
    const QString base = measure();
    std::fprintf(stderr, "[%s] sidebar %d px (fixed %d), box slot %d px\n",
                 tag, side_->width(), width_, stack_->width());
    for (ContainerWidget* c : boxes_->all())
        if (c->minimumSizeHint().width() > stack_->width())
            std::fprintf(stderr, "[%s] box %s wants %d px > slot %d (clipped)\n", tag,
                         qPrintable(c->id()), c->minimumSizeHint().width(), stack_->width());
    bool ok = side_->width() == width_;
    if (qEnvironmentVariableIntValue("TTC_PANELCHECK")) {
        // Each case must leave sidebar / slot / window minimum where they were.
        const auto check = [&](const char* what) {
            settle();
            const QString m = measure();
            std::fprintf(stderr, "[%s] %-18s %s%s\n", tag, what, qPrintable(m),
                         m == base ? "" : "  <-- MOVED");
            ok = ok && m == base;
        };
        const auto all = boxes_->all();
        for (auto* c : all) boxes_->setContainerVisible(c->id(), false);
        check("all hidden");
        for (auto* c : all) boxes_->setContainerVisible(c->id(), true);
        check("all shown");
        for (auto* c : all) boxes_->floatContainer(c->id());
        check("all floating");
        for (auto* c : all) boxes_->dockContainer(c->id());
        check("all docked");
        QToolButton* extra[3];
        for (auto*& b : extra) b = addToggle("A LONG DUMMY LABEL");
        check("3 extra toggles");
        for (auto* b : extra) delete b;
        cells_.resize(cells_.size() - 3);
        relayoutGrid();
    }
    std::fprintf(stderr, "[%s] sidebar width %s\n", tag, ok ? "OK" : "FAIL");
}

QWidget* SideBar::dropBefore(const QPoint& p) const {
    for (int i = 0; i < boxCol_->count(); ++i) {
        auto* c = qobject_cast<ContainerWidget*>(boxCol_->itemAt(i)->widget());
        if (!c || !c->isVisible()) continue;
        if (p.y() < c->geometry().center().y()) return c;
    }
    return nullptr;
}

void SideBar::showDropAt(const QPoint& p) {
    const QRect cr = boxCol_->geometry();
    int y = cr.top();
    if (QWidget* before = dropBefore(p)) {
        y = before->geometry().top() - 3;
    } else {
        for (int i = boxCol_->count() - 1; i >= 0; --i) {
            auto* c = qobject_cast<ContainerWidget*>(boxCol_->itemAt(i)->widget());
            if (c && c->isVisible()) { y = c->geometry().bottom() + 3; break; }
        }
    }
    dropLine_->setGeometry(cr.left(), y - 1, cr.width(), 2);
    dropLine_->raise();
    dropLine_->show();
}

void SideBar::hideDrop() { dropLine_->hide(); }

bool SideBar::dropBox(const QString& id, const QPoint& p) {
    ContainerWidget* box = boxes_->container(id);
    if (!box || box->isFloating()) return false;
    QWidget* before = dropBefore(p);
    if (before == box) return false;               // dropped on itself
    boxCol_->removeWidget(box);
    // The column ends in a stretch: "the end" is just before it.
    const int at = before ? boxCol_->indexOf(before) : boxCol_->count() - 1;
    boxCol_->insertWidget(at, box);
    saveOrder();
    return true;
}

void SideBar::saveOrder() const {
    QStringList ids;
    for (int i = 0; i < boxCol_->count(); ++i)
        if (auto* c = qobject_cast<ContainerWidget*>(boxCol_->itemAt(i)->widget()))
            ids << c->id();
    QSettings s;
    s.setValue(prefix_ + "order/boxes", ids);
    s.remove(prefix_ + "order/rx");    // Panel's old second column, now merged
}

void SideBar::restoreOrder() {
    QSettings s;
    if (!s.contains(prefix_ + "order/boxes")) return;   // never rearranged
    // An order saved by Panel's two-column sidebar: its receive column follows.
    const QStringList ids = s.value(prefix_ + "order/boxes").toStringList()
                            + s.value(prefix_ + "order/rx").toStringList();
    // Boxes the saved order doesn't name (new ones) stay where built.
    for (const QString& id : ids) {
        ContainerWidget* c = boxes_->container(id);
        if (!c || boxCol_->indexOf(c) < 0) continue;   // lacking, or floating
        boxCol_->removeWidget(c);
        boxCol_->insertWidget(boxCol_->count() - 1, c);   // before the stretch
    }
}

} // namespace ttc
