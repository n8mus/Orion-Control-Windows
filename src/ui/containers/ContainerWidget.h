// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AetherSDR (src/gui/containers, GPL-3.0) — trimmed: docked or
// floating only (no canvas), one content widget per box; the title-bar drag
// is theirs (drop targets rearrange on it).
#pragma once
#include <QHash>
#include <QSize>
#include <QString>
#include <QWidget>

class QVBoxLayout;

namespace ttc {

class ContainerTitleBar;

// A titled box for the Panel layout: title bar on top, one content widget
// below. It lives docked in some box layout, or floats in its own window
// (ContainerManager moves it between the two).
class ContainerWidget : public QWidget {
    Q_OBJECT
public:
    enum class DockMode { Docked, Floating };
    // Drag payload of a docked box's title bar: the box id.
    static constexpr const char* kDragMime = "application/x-ttc-box";

    ContainerWidget(const QString& id, const QString& title, QWidget* parent = nullptr);

    QString id() const { return id_; }
    void setTitle(const QString& title);
    QString title() const;

    // Installs `content` (the previous one is unparented and returned).
    QWidget* setContent(QWidget* content);
    QWidget* content() const { return content_; }

    DockMode dockMode() const { return mode_; }
    bool isFloating() const { return mode_ == DockMode::Floating; }

    // The operator's show/hide (the × button), separate from Qt visibility.
    void setContainerVisible(bool visible);
    bool isContainerVisible() const { return visible_; }

    ContainerTitleBar* titleBar() const { return bar_; }
    void setTitleBarVisible(bool visible);
    void setDefaultFloatingSize(const QSize& s) { floatSize_ = s; }
    QSize defaultFloatingSize() const { return floatSize_; }

signals:
    void floatRequested();
    void dockRequested();
    void closeRequested();
    void alwaysOnTopToggled(bool on);
    void visibilityChanged(bool visible);
    void dockModeChanged(DockMode mode);

private:
    friend class ContainerManager;
    friend class FloatingContainerWindow;
    void setDockMode(DockMode mode);

    QString id_;
    ContainerTitleBar* bar_;
    QWidget* body_;
    QVBoxLayout* bodyLay_;
    QWidget* content_ = nullptr;
    DockMode mode_ = DockMode::Docked;
    bool visible_ = true;
    QSize floatSize_;
    int savedMaxWidth_ = -1;   // content's docked maximumWidth while floating
};

} // namespace ttc
