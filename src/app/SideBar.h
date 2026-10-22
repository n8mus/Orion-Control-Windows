// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>

class QGridLayout;
class QScrollArea;
class QToolButton;
class QVBoxLayout;

namespace ttc {

class ContainerManager;
class ContainerWidget;

// The right-hand sidebar both window layouts share (AetherSDR's
// AppletPanel idea): ONE fixed width that its contents are sized by, a
// button grid with a fixed column count on top, then a single column of
// boxes (ui/containers: float, dock, hide, drag to rearrange) in a scroll
// area, and an optional footer pinned under it. The arrangement is kept
// in QSettings under `prefix` ("panel/", "classic/side/"), so each layout
// remembers its own.
class SideBar : public QObject {
    Q_OBJECT
public:
    SideBar(QWidget* window, QWidget* parent, const QString& prefix,
            int width, int gridCols);
    ~SideBar() override;

    QWidget* widget() const { return side_; }
    int fixedWidth() const { return width_; }
    ContainerManager* boxes() const { return boxes_; }
    QVBoxLayout* column() const { return boxCol_; }

    // A docked box at the column's end holding `body`, plus its show/hide
    // toggle in the grid labelled `toggle`. An EMPTY `toggle` leaves the
    // grid alone: something else owns that box's visibility (SOLAR is the
    // DISPLAY panel's "Solar data (NOAA)" tick).
    ContainerWidget* addBox(const QString& id, const QString& title,
                            const QString& toggle, QWidget* body);
    // A show/hide-style toggle in the next grid cell (unwired).
    QToolButton* addToggle(const QString& text);
    // Any button (menu buttons, tools) in the next grid cell; `name` is how
    // the DISPLAY panel's picker lists it. Sized by the grid, never wider.
    // `at` = its cell index (default: after the last).
    void addButton(const QString& name, QWidget* w, int at = -1);
    // Rearrange the grid: the named buttons first, in this order; anything
    // not named keeps its relative order after them.
    void setOrder(const QStringList& names);
    // The grid's buttons by name, in grid order (the DISPLAY picker's list).
    QStringList buttonNames() const;
    // Buttons the operator unticked: out of the grid until ticked again.
    // A box's toggle going out hides its box too.
    void setHidden(const QStringList& names);
    // A "▲ BUTTONS" bar over the grid that folds it away (state in
    // prefix + "buttonsOpen").
    void addCollapseBar();
    // A widget pinned under the scroll area (Classic's clock).
    void setFooter(QWidget* w);
    // After the last box: spare height collects at the column's end.
    void endColumn();
    // Everything placed: saved order, then saved box modes/visibility.
    void restore();

    // Drag-to-rearrange (their AppletPanel drop logic): where a box dropped
    // at `p` (column coordinates) lands, the indicator there, the move.
    void showDropAt(const QPoint& p);
    void hideDrop();
    bool dropBox(const QString& id, const QPoint& p);

    // Selftest: prints the width as "[tag] sidebar …" and, with
    // TTC_PANELCHECK=1 (throwaway XDG_CONFIG_HOME: it floats and hides
    // boxes), proves nothing inside can move it.
    void checkWidth(const char* tag);

protected:
    bool eventFilter(QObject* o, QEvent* ev) override;

private:
    struct Cell { QString name; QWidget* w; QString boxId; };
    void relayoutGrid();
    QWidget* dropBefore(const QPoint& p) const;
    void saveOrder() const;
    void restoreOrder();

    QWidget* window_;
    QString prefix_;
    int width_;
    int cols_;
    QWidget* side_;
    QVBoxLayout* sideLay_;
    QWidget* gridHost_;
    QGridLayout* grid_;
    QScrollArea* scroll_;
    QWidget* stack_;
    QVBoxLayout* boxCol_;
    QWidget* dropLine_ = nullptr;
    ContainerManager* boxes_;
    QVector<Cell> cells_;
    QSet<QString> hidden_;
};

} // namespace ttc
