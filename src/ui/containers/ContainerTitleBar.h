// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AetherSDR (src/gui/containers, GPL-3.0) — trimmed: no canvas
// mode, no theme manager (fixed colours).
#pragma once
#include <QPoint>
#include <QWidget>

class QLabel;
class QPushButton;

namespace ttc {

// The 18 px gradient strip on top of every box: grip, title, and the
// pin (always on top, floating only), float/dock and close buttons.
class ContainerTitleBar : public QWidget {
    Q_OBJECT
public:
    static constexpr int kHeight = 18;

    explicit ContainerTitleBar(const QString& title, QWidget* parent = nullptr);

    void setTitle(const QString& title);
    QString title() const;
    void setFloatingState(bool floating);
    void setAlwaysOnTopState(bool on);   // no signal
    void setCloseButtonVisible(bool visible);

    QPushButton* floatButton() const { return floatBtn_; }
    QPushButton* closeButton() const { return closeBtn_; }
    QPushButton* pinButton() const { return pinBtn_; }

signals:
    void floatToggleClicked();
    void closeClicked();
    void alwaysOnTopToggled(bool on);
    // A docked bar dragged past the threshold (the Panel layout's reorder
    // hook; nothing listens yet).
    void dragStartRequested(const QPoint& globalPos);

protected:
    void mousePressEvent(QMouseEvent* ev) override;
    void mouseMoveEvent(QMouseEvent* ev) override;
    void mouseReleaseEvent(QMouseEvent* ev) override;

private:
    QLabel* title_;
    QPushButton* pinBtn_;
    QPushButton* floatBtn_;
    QPushButton* closeBtn_;
    bool floating_ = false;
    bool closeAllowed_ = true;
    bool pressed_ = false;
    QPoint pressPos_;
};

} // namespace ttc
