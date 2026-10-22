// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AetherSDR (src/gui/containers, GPL-3.0) — see the header.
#include "ui/containers/ContainerManager.h"

#include <QBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

#include "ui/containers/ContainerTitleBar.h"
#include "ui/containers/ContainerWidget.h"
#include "ui/containers/FloatingContainerWindow.h"

namespace ttc {

namespace {
constexpr int kSchemaVersion = 1;

// The box layout (anywhere under `l`) that directly holds `w`.
QBoxLayout* findBox(QLayout* l, QWidget* w) {
    if (!l) return nullptr;
    if (auto* box = qobject_cast<QBoxLayout*>(l); box && box->indexOf(w) >= 0) return box;
    for (int i = 0; i < l->count(); ++i)
        if (QLayout* sub = l->itemAt(i)->layout())
            if (QBoxLayout* hit = findBox(sub, w)) return hit;
    return nullptr;
}
}  // namespace

ContainerManager::ContainerManager(QWidget* window, const QString& prefix, QObject* parent)
    : QObject(parent), window_(window), prefix_(prefix) {}

ContainerManager::~ContainerManager() {
    for (auto* w : std::as_const(floating_)) {
        w->releaseContainer();
        w->deleteLater();
    }
}

QString ContainerManager::key(const char* what, const QString& id) const {
    QString safe = id;
    safe.replace('/', '_');
    return prefix_ + what + "/" + safe;
}

ContainerWidget* ContainerManager::create(const QString& id, const QString& title,
                                          QWidget* content) {
    if (auto* c = container(id)) return c;
    auto* c = new ContainerWidget(id, title);
    if (content) c->setContent(content);
    boxes_.insert(id, c);
    wire(c);
    return c;
}

ContainerWidget* ContainerManager::container(const QString& id) const {
    return boxes_.value(id).data();
}

FloatingContainerWindow* ContainerManager::floatingWindow(const QString& id) const {
    return floating_.value(id, nullptr);
}

QList<ContainerWidget*> ContainerManager::all() const {
    QList<ContainerWidget*> out;
    for (const auto& p : boxes_)
        if (p) out.append(p.data());
    return out;
}

void ContainerManager::wire(ContainerWidget* c) {
    const QString id = c->id();
    connect(c, &ContainerWidget::floatRequested, this, [this, id] { floatContainer(id); });
    connect(c, &ContainerWidget::dockRequested, this, [this, id] { dockContainer(id); });
    connect(c, &ContainerWidget::closeRequested, this,
            [this, id] { setContainerVisible(id, false); });
    connect(c, &ContainerWidget::alwaysOnTopToggled, this, [this, id](bool on) {
        QSettings().setValue(key("ontop", id), on);
        if (auto* w = floating_.value(id, nullptr)) w->setAlwaysOnTop(on);
    });
}

void ContainerManager::floatContainer(const QString& id) {
    ContainerWidget* c = container(id);
    if (!c || c->isFloating()) return;

    // Remember the slot so docking puts it back exactly where it was.
    Slot slot;
    if (QWidget* owner = c->parentWidget()) {
        slot.owner = owner;
        if (QBoxLayout* box = findBox(owner->layout(), c)) {
            slot.box = box;
            slot.index = box->indexOf(c);
            slot.stretch = box->stretch(slot.index);
            box->removeWidget(c);
        }
        c->setParent(nullptr);
    }
    slots_.insert(id, slot);

    auto* win = new FloatingContainerWindow(window_ ? window_->window() : nullptr);
    win->setGeometryKey(key("geom", id));
    win->takeContainer(c);
    connect(win, &FloatingContainerWindow::dockRequested, this,
            [this, id](ContainerWidget*) { dockContainer(id); });
    floating_.insert(id, win);
    win->restoreAndEnsureVisible(window_);
    const bool onTop = QSettings().value(key("ontop", id), false).toBool();
    if (onTop) win->setAlwaysOnTop(true);
    c->titleBar()->setAlwaysOnTopState(onTop);
    if (c->isContainerVisible()) win->show();
    saveState();
}

void ContainerManager::dockContainer(const QString& id) {
    ContainerWidget* c = container(id);
    if (!c || !c->isFloating()) return;
    FloatingContainerWindow* win = floating_.take(id);
    if (!win) return;
    ContainerWidget* back = win->releaseContainer();
    win->hide();
    win->deleteLater();
    if (!back) return;
    const Slot slot = slots_.take(id);
    if (slot.owner) {
        back->setParent(slot.owner);
        if (slot.box) {
            const int i = std::clamp(slot.index, 0, slot.box->count());
            slot.box->insertWidget(i, back, slot.stretch);
        }
        back->setVisible(back->isContainerVisible());
    }
    saveState();
}

void ContainerManager::setContainerVisible(const QString& id, bool visible) {
    ContainerWidget* c = container(id);
    if (!c) return;
    c->setContainerVisible(visible);
    if (auto* w = floating_.value(id, nullptr)) w->setVisible(visible);
    saveState();
}

void ContainerManager::saveState() const {
    if (restoring_) return;
    QJsonObject boxes;
    for (auto it = boxes_.constBegin(); it != boxes_.constEnd(); ++it) {
        if (!it.value()) continue;
        QJsonObject e;
        e["mode"] = it.value()->isFloating() ? "floating" : "docked";
        e["visible"] = it.value()->isContainerVisible();
        boxes[it.key()] = e;
    }
    QJsonObject root;
    root["version"] = kSchemaVersion;
    root["containers"] = boxes;
    QSettings().setValue(prefix_ + "containers",
                         QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
    emit const_cast<ContainerManager*>(this)->stateChanged();
}

void ContainerManager::restoreState() {
    const QJsonDocument doc = QJsonDocument::fromJson(
        QSettings().value(prefix_ + "containers").toString().toUtf8());
    if (!doc.isObject() || doc.object().value("version").toInt() != kSchemaVersion) return;
    restoring_ = true;
    const QJsonObject boxes = doc.object().value("containers").toObject();
    for (auto it = boxes.constBegin(); it != boxes.constEnd(); ++it) {
        if (!container(it.key())) continue;
        const QJsonObject e = it.value().toObject();
        setContainerVisible(it.key(), e.value("visible").toBool(true));
        if (e.value("mode").toString() == "floating") floatContainer(it.key());
        else dockContainer(it.key());
    }
    restoring_ = false;
}

void ContainerManager::prepareShutdown() {
    saveState();
    for (auto* w : std::as_const(floating_)) w->prepareShutdown();
}

} // namespace ttc
