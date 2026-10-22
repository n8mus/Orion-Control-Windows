// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AetherSDR (src/gui/containers, GPL-3.0) — see the header.
#include "ui/containers/ContainerWidget.h"

#include <QDrag>
#include <QMimeData>
#include <QPixmap>
#include <QVBoxLayout>

#include "ui/containers/ContainerTitleBar.h"

namespace ttc {

ContainerWidget::ContainerWidget(const QString& id, const QString& title, QWidget* parent)
    : QWidget(parent), id_(id) {
    setObjectName(id);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    bar_ = new ContainerTitleBar(title, this);
    outer->addWidget(bar_);
    body_ = new QWidget(this);
    bodyLay_ = new QVBoxLayout(body_);
    bodyLay_->setContentsMargins(0, 0, 0, 0);
    bodyLay_->setSpacing(0);
    outer->addWidget(body_, 1);

    connect(bar_, &ContainerTitleBar::floatToggleClicked, this, [this] {
        if (isFloating()) emit dockRequested();
        else emit floatRequested();
    });
    connect(bar_, &ContainerTitleBar::closeClicked, this, &ContainerWidget::closeRequested);
    connect(bar_, &ContainerTitleBar::alwaysOnTopToggled, this,
            &ContainerWidget::alwaysOnTopToggled);
    // Theirs: dragging a docked bar carries the box id to a drop target
    // (the Panel sidebar rearranges on it), the bar's picture as the cursor.
    connect(bar_, &ContainerTitleBar::dragStartRequested, this, [this] {
        auto* drag = new QDrag(bar_);
        auto* mime = new QMimeData;
        mime->setData(kDragMime, id_.toUtf8());
        drag->setMimeData(mime);
        QPixmap pm(bar_->size());
        pm.fill(Qt::transparent);
        bar_->render(&pm);
        drag->setPixmap(pm);
        drag->setHotSpot(QPoint(bar_->width() / 2, bar_->height() / 2));
        drag->exec(Qt::MoveAction);
    });
}

void ContainerWidget::setTitle(const QString& title) { bar_->setTitle(title); }
QString ContainerWidget::title() const { return bar_->title(); }

QWidget* ContainerWidget::setContent(QWidget* content) {
    QWidget* prev = content_;
    if (prev) {
        if (savedMaxWidth_ >= 0) prev->setMaximumWidth(savedMaxWidth_);
        savedMaxWidth_ = -1;
        bodyLay_->removeWidget(prev);
        prev->setParent(nullptr);
    }
    content_ = content;
    if (content_) {
        content_->setParent(body_);
        bodyLay_->addWidget(content_, 1);
        content_->show();
        if (isFloating()) {
            savedMaxWidth_ = content_->maximumWidth();
            content_->setMaximumWidth(QWIDGETSIZE_MAX);
        }
    }
    return prev;
}

void ContainerWidget::setTitleBarVisible(bool visible) { bar_->setVisible(visible); }

void ContainerWidget::setContainerVisible(bool visible) {
    if (visible == visible_) return;
    visible_ = visible;
    setVisible(visible);
    emit visibilityChanged(visible);
}

void ContainerWidget::setDockMode(DockMode mode) {
    if (mode == mode_) return;
    mode_ = mode;
    bar_->setFloatingState(mode == DockMode::Floating);
    // A sidebar may cap a box's width; a floating window should not.
    if (content_) {
        if (mode == DockMode::Floating) {
            savedMaxWidth_ = content_->maximumWidth();
            content_->setMaximumWidth(QWIDGETSIZE_MAX);
        } else if (savedMaxWidth_ >= 0) {
            content_->setMaximumWidth(savedMaxWidth_);
            savedMaxWidth_ = -1;
        }
    }
    emit dockModeChanged(mode);
}

} // namespace ttc
