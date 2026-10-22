// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AetherSDR (src/gui/containers, GPL-3.0) — trimmed: a flat
// registry (no nested boxes, no content factories, no canvas), QSettings.
#pragma once
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>

class QBoxLayout;
class QWidget;

namespace ttc {

class ContainerWidget;
class FloatingContainerWindow;

// Owns the Panel layout's boxes: floats them into their own windows and
// docks them back into the exact layout slot they left, and remembers
// per box (in QSettings under `prefix`): docked/floating, shown/hidden,
// window geometry and the always-on-top pin.
class ContainerManager : public QObject {
    Q_OBJECT
public:
    // `window` is the main window (floating windows are its transients and
    // open on its screen); keys go under `prefix` ("panel/").
    explicit ContainerManager(QWidget* window, const QString& prefix = "panel/",
                              QObject* parent = nullptr);
    ~ContainerManager() override;

    // A new docked box holding `content`; the caller puts it in a layout.
    ContainerWidget* create(const QString& id, const QString& title, QWidget* content);
    ContainerWidget* container(const QString& id) const;
    FloatingContainerWindow* floatingWindow(const QString& id) const;
    QList<ContainerWidget*> all() const;

    void floatContainer(const QString& id);
    void dockContainer(const QString& id);
    void setContainerVisible(const QString& id, bool visible);

    void saveState() const;
    // Re-applies the saved modes/visibility to boxes already created and
    // placed (unknown ids in the saved state are ignored).
    void restoreState();
    // Application exit: save, close floating windows without docking.
    void prepareShutdown();

signals:
    void stateChanged();

private:
    struct Slot {
        QPointer<QWidget> owner;      // widget whose layout tree held the box
        QPointer<QBoxLayout> box;     // the box layout itself
        int index = -1;
        int stretch = 0;
    };
    void wire(ContainerWidget* c);
    QString key(const char* what, const QString& id) const;

    QWidget* window_;
    QString prefix_;
    QHash<QString, QPointer<ContainerWidget>> boxes_;
    QHash<QString, Slot> slots_;
    QHash<QString, FloatingContainerWindow*> floating_;
    bool restoring_ = false;
};

} // namespace ttc
