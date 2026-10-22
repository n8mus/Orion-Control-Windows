// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AetherSDR (src/gui/containers, GPL-3.0) — see the header.
#include "ui/containers/ContainerTitleBar.h"

#include <QGuiApplication>
#include <QTimer>

#include "ui/containers/FramelessMoveHelper.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QSignalBlocker>

namespace ttc {

namespace {
constexpr const char* kBarStyle =
    "QWidget { background: qlineargradient(x1:0,y1:0,x2:0,y2:1,"
    " stop:0 #5a7494, stop:0.5 #384e68, stop:1 #1e2e3e);"
    " border-bottom: 1px solid #0a1a28; }";
constexpr const char* kTitleStyle =
    "QLabel { background: transparent; color: #e0ecf4; font-size: 10px;"
    " font-weight: bold; border: none; }";
constexpr const char* kBtnStyle =
    "QPushButton { background: transparent; border: none; color: #c8d8e8;"
    " font-size: 11px; font-weight: bold; padding: 0px 4px; }"
    "QPushButton:hover { color: #ffffff; }"
    "QPushButton:checked { color: #ffd166; }";
constexpr const char* kFloatBtnStyle =
    "QPushButton { background: transparent; border: none; color: #c8d8e8;"
    " font-size: 15px; font-weight: bold; padding: 0px 4px; }"
    "QPushButton:hover { color: #ffffff; }";
constexpr int kDragThresholdPx = 6;

const QString kFloatGlyph = QString::fromUtf8("\xe2\xa7\x89");   // ⧉
const QString kDockGlyph = QString::fromUtf8("\xe2\x86\x99");    // ↙
}  // namespace

ContainerTitleBar::ContainerTitleBar(const QString& title, QWidget* parent)
    : QWidget(parent) {
    setFixedHeight(kHeight);
    setCursor(Qt::OpenHandCursor);
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(kBarStyle);
    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(6, 0, 2, 0);
    lay->setSpacing(4);

    auto* grip = new QLabel(QString::fromUtf8("\xe2\x8b\xae\xe2\x8b\xae"), this);  // ⋮⋮
    grip->setStyleSheet("QLabel { background: transparent; color: #8fa3b8;"
                        " font-size: 10px; border: none; }");
    lay->addWidget(grip);
    title_ = new QLabel(title, this);
    title_->setStyleSheet(kTitleStyle);
    lay->addWidget(title_);
    lay->addStretch();

    pinBtn_ = new QPushButton(QString::fromUtf8("\xf0\x9f\x93\x8c"), this);  // 📌
    pinBtn_->setStyleSheet(kBtnStyle);
    pinBtn_->setFixedSize(16, 16);
    pinBtn_->setCheckable(true);
    pinBtn_->setToolTip("Always on top");
    pinBtn_->setCursor(Qt::ArrowCursor);
    pinBtn_->setFocusPolicy(Qt::NoFocus);
    pinBtn_->setVisible(false);   // floating only
    connect(pinBtn_, &QPushButton::toggled, this, &ContainerTitleBar::alwaysOnTopToggled);
    lay->addWidget(pinBtn_);

    floatBtn_ = new QPushButton(kFloatGlyph, this);
    floatBtn_->setStyleSheet(kFloatBtnStyle);
    floatBtn_->setFixedSize(18, 18);
    floatBtn_->setToolTip("Pop out into a floating window");
    floatBtn_->setCursor(Qt::ArrowCursor);
    floatBtn_->setFocusPolicy(Qt::NoFocus);
    connect(floatBtn_, &QPushButton::clicked, this, &ContainerTitleBar::floatToggleClicked);
    lay->addWidget(floatBtn_);

    closeBtn_ = new QPushButton(QString::fromUtf8("\xc3\x97"), this);  // ×
    closeBtn_->setStyleSheet(kBtnStyle);
    closeBtn_->setFixedSize(16, 16);
    closeBtn_->setToolTip("Hide this box");
    closeBtn_->setCursor(Qt::ArrowCursor);
    closeBtn_->setFocusPolicy(Qt::NoFocus);
    connect(closeBtn_, &QPushButton::clicked, this, &ContainerTitleBar::closeClicked);
    lay->addWidget(closeBtn_);
}

void ContainerTitleBar::setTitle(const QString& title) { title_->setText(title); }
QString ContainerTitleBar::title() const { return title_->text(); }

void ContainerTitleBar::setFloatingState(bool floating) {
    floating_ = floating;
    floatBtn_->setText(floating ? kDockGlyph : kFloatGlyph);
    floatBtn_->setToolTip(floating ? "Return to panel" : "Pop out into a floating window");
    // A floating box closes with its window's own close button (= dock).
    closeBtn_->setVisible(closeAllowed_ && !floating);
    pinBtn_->setVisible(floating);
}

void ContainerTitleBar::setAlwaysOnTopState(bool on) {
    QSignalBlocker b(pinBtn_);
    pinBtn_->setChecked(on);
}

void ContainerTitleBar::setCloseButtonVisible(bool visible) {
    closeAllowed_ = visible;
    closeBtn_->setVisible(visible && !floating_);
}

// Their handlers minus the canvas: a floating bar moves its (frameless)
// window via FramelessMoveHelper; a docked bar starts a drag past 6 px.
void ContainerTitleBar::mousePressEvent(QMouseEvent* ev) {
    if (ev->button() != Qt::LeftButton) { QWidget::mousePressEvent(ev); return; }
    if (floating_) {
        FramelessMoveHelper::start(this, ev);
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    pressed_ = true;
    pressPos_ = ev->globalPosition().toPoint();
    setCursor(Qt::ClosedHandCursor);
    ev->accept();
}

void ContainerTitleBar::mouseMoveEvent(QMouseEvent* ev) {
    if (floating_ && FramelessMoveHelper::move(this, ev)) return;
    if (!pressed_ || !(ev->buttons() & Qt::LeftButton)) { QWidget::mouseMoveEvent(ev); return; }
    const QPoint g = ev->globalPosition().toPoint();
    if ((g - pressPos_).manhattanLength() < kDragThresholdPx) return;
    emit dragStartRequested(g);
    pressed_ = false;
    setCursor(Qt::OpenHandCursor);
}

void ContainerTitleBar::mouseReleaseEvent(QMouseEvent* ev) {
    if (floating_) {
        FramelessMoveHelper::finish(this, ev);
        setCursor(Qt::OpenHandCursor);
        return;
    }
    QWidget::mouseReleaseEvent(ev);
    if (ev->button() == Qt::LeftButton) {
        QTimer::singleShot(0, this, [this]() {
            if (!floating_ && pressed_
                && !(QGuiApplication::mouseButtons() & Qt::LeftButton)) {
                pressed_ = false;
                setCursor(Qt::OpenHandCursor);
            }
        });
    }
}

} // namespace ttc
