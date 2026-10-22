// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AetherSDR (src/gui/containers, GPL-3.0) — see the header.
#include "ui/containers/FloatingContainerWindow.h"

#include <QCloseEvent>
#include <QGuiApplication>
#include <QScreen>
#include <QSettings>
#include <QVBoxLayout>

#include "ui/containers/ContainerWidget.h"
#include "ui/containers/FramelessResizer.h"

namespace ttc {

namespace {
constexpr int kDefaultW = 300;
}

FloatingContainerWindow::FloatingContainerWindow(QWidget* parent)
    : QWidget(parent, Qt::Window | Qt::FramelessWindowHint) {
    // Frameless as in AetherSDR (their default): the box's own dark title
    // bar is the window's, moved by FramelessMoveHelper, edges resized by
    // FramelessResizer — on X11 both do it by hand (the WM grab is
    // unreliable there). A WM frame came out white on a light desktop.
    setAttribute(Qt::WA_DeleteOnClose, false);
    setAttribute(Qt::WA_QuitOnClose, false);
    // The console's dark look (a new top-level doesn't inherit it): an
    // objectName rule — a class-name rule never matches a namespaced class.
    setObjectName("floatWin");
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, QColor(0x0f, 0x14, 0x1b));
    pal.setColor(QPalette::WindowText, QColor(0xc8, 0xd4, 0xe0));
    setPalette(pal);
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet("#floatWin { background: #0f141b; border: 1px solid #3a4a5e; }");
    lay_ = new QVBoxLayout(this);
    lay_->setContentsMargins(1, 1, 1, 1);   // the border
    lay_->setSpacing(0);
    saveTimer_.setSingleShot(true);
    saveTimer_.setInterval(400);
    connect(&saveTimer_, &QTimer::timeout, this, &FloatingContainerWindow::saveGeometryNow);
    FramelessResizer::install(this);
}

void FloatingContainerWindow::takeContainer(ContainerWidget* c) {
    if (c_ == c) return;
    if (c_) releaseContainer();
    c_ = c;
    if (!c_) return;
    destroyedConn_ = connect(c_, &QObject::destroyed, this, [this] { c_ = nullptr; });
    c_->setParent(this);
    lay_->addWidget(c_, 1);
    c_->setDockMode(ContainerWidget::DockMode::Floating);
    c_->show();
    setWindowTitle(c_->title());
}

ContainerWidget* FloatingContainerWindow::releaseContainer() {
    ContainerWidget* c = c_;
    if (!c) return nullptr;
    disconnect(destroyedConn_);
    c_ = nullptr;
    lay_->removeWidget(c);
    c->setParent(nullptr);
    c->setDockMode(ContainerWidget::DockMode::Docked);
    return c;
}

void FloatingContainerWindow::restoreAndEnsureVisible(QWidget* anchor) {
    restoring_ = true;
    bool restored = false;
    if (!geomKey_.isEmpty()) {
        const QByteArray g = QByteArray::fromBase64(QSettings().value(geomKey_).toByteArray());
        restored = !g.isEmpty() && restoreGeometry(g);
    }
    QScreen* screen = anchor && anchor->screen() ? anchor->screen()
                                                 : QGuiApplication::primaryScreen();
    if (!restored) {
        adjustSize();
        const QSize def = c_ ? c_->defaultFloatingSize() : QSize();
        const QSize hint = def.isValid() ? def : sizeHint();
        const int w = std::max(kDefaultW, hint.width());
        const int h = std::max(hint.height(), 80);
        resize(w, h);
        if (screen) {
            const QRect a = screen->availableGeometry();
            move(a.center().x() - w / 2, a.center().y() - h / 2);
        }
    } else {
        // A monitor that has since gone away: bring it back on screen.
        bool onScreen = false;
        for (QScreen* s : QGuiApplication::screens())
            if (s->availableGeometry().contains(geometry().topLeft())) { onScreen = true; break; }
        if (!onScreen && screen) {
            const QRect a = screen->availableGeometry();
            move(a.center().x() - width() / 2, a.center().y() - height() / 2);
        }
    }
    restoring_ = false;
}

void FloatingContainerWindow::saveGeometryNow() const {
    if (geomKey_.isEmpty()) return;
    QSettings().setValue(geomKey_, saveGeometry().toBase64());
}

void FloatingContainerWindow::prepareShutdown() {
    saveTimer_.stop();
    saveGeometryNow();
    shuttingDown_ = true;
    close();
}

void FloatingContainerWindow::setAlwaysOnTop(bool on) {
    if (onTop_ == on) return;
    onTop_ = on;
    const bool wasVisible = isVisible();
    const QRect g = geometry();
    setWindowFlag(Qt::WindowStaysOnTopHint, on);   // hides the window
    if (wasVisible) {
        setGeometry(g);
        show();
    }
}

void FloatingContainerWindow::closeEvent(QCloseEvent* ev) {
    if (!shuttingDown_ && c_) {
        saveTimer_.stop();
        saveGeometryNow();
        emit dockRequested(c_);
    }
    QWidget::closeEvent(ev);
}

void FloatingContainerWindow::moveEvent(QMoveEvent* ev) {
    QWidget::moveEvent(ev);
    if (!restoring_) saveTimer_.start();
}

void FloatingContainerWindow::resizeEvent(QResizeEvent* ev) {
    QWidget::resizeEvent(ev);
    if (!restoring_) saveTimer_.start();
}

} // namespace ttc
