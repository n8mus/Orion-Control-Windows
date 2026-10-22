// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AetherSDR (src/gui/containers, GPL-3.0) — trimmed: always
// frameless (their default), QSettings for the geometry.
#pragma once
#include <QString>
#include <QTimer>
#include <QWidget>

class QVBoxLayout;

namespace ttc {

class ContainerWidget;

// The top-level window a floated box lives in. Its geometry (size, place,
// monitor) is saved under a per-box key a moment after every move/resize;
// closing it with the window manager's × docks the box back.
class FloatingContainerWindow : public QWidget {
    Q_OBJECT
public:
    explicit FloatingContainerWindow(QWidget* parent = nullptr);

    void takeContainer(ContainerWidget* c);
    ContainerWidget* releaseContainer();
    ContainerWidget* container() const { return c_; }

    void setGeometryKey(const QString& key) { geomKey_ = key; }
    // Saved geometry if it still lands on a screen, else centred on
    // `anchor`'s screen at the box's default size.
    void restoreAndEnsureVisible(QWidget* anchor);
    void setAlwaysOnTop(bool on);
    bool isAlwaysOnTop() const { return onTop_; }
    // Save now and close without docking (application exit).
    void prepareShutdown();

signals:
    void dockRequested(ContainerWidget* c);

protected:
    void closeEvent(QCloseEvent* ev) override;
    void moveEvent(QMoveEvent* ev) override;
    void resizeEvent(QResizeEvent* ev) override;

private:
    void saveGeometryNow() const;

    ContainerWidget* c_ = nullptr;
    QMetaObject::Connection destroyedConn_;
    QVBoxLayout* lay_;
    QString geomKey_;
    QTimer saveTimer_;
    bool restoring_ = false;
    bool shuttingDown_ = false;
    bool onTop_ = false;
};

} // namespace ttc
