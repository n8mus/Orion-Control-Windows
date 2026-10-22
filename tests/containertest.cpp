// SPDX-License-Identifier: GPL-3.0-or-later
// Panel-layout boxes (src/ui/containers, ported from AetherSDR), offscreen:
// float/dock back into the same slot, hide, pin, geometry + state saved and
// restored by a fresh manager. Settings go to a temp dir, never the real ones.
//   cmake --build build --target containertest
//   QT_QPA_PLATFORM=offscreen ./build/containertest
#include <QApplication>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include <cstdio>

#include "ui/containers/ContainerManager.h"
#include "ui/containers/ContainerTitleBar.h"
#include "ui/containers/ContainerWidget.h"
#include "ui/containers/FloatingContainerWindow.h"

using namespace ttc;

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++fails;
}

static void settle() {
    for (int i = 0; i < 5; ++i) QApplication::processEvents();
}

int main(int argc, char** argv) {
    QTemporaryDir cfg;
    qputenv("XDG_CONFIG_HOME", cfg.path().toLocal8Bit());
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("ttc-test");
    QCoreApplication::setApplicationName("containertest");

    QWidget win;
    auto* outer = new QVBoxLayout(&win);
    auto* side = new QVBoxLayout;          // nested: the slot is not win's own layout
    outer->addLayout(side);
    auto* top = new QLabel("top", &win);
    side->addWidget(top);
    win.resize(400, 600);

    {
        ContainerManager mgr(&win);
        auto* a = mgr.create("vfoA", "VFO A", new QLabel("A"));
        auto* b = mgr.create("log", "LOG", new QLabel("log"));
        a->content()->setMaximumWidth(250);
        side->addWidget(a, 2);
        side->addWidget(b);
        win.show();
        settle();
        check(mgr.create("vfoA", "again", nullptr) == a, "create() is idempotent per id");
        check(side->indexOf(a) == 1 && !a->isFloating(), "box starts docked in its slot");

        a->titleBar()->floatButton()->click();
        settle();
        auto* fw = mgr.floatingWindow("vfoA");
        check(a->isFloating() && fw && fw->container() == a, "float button floats into a window");
        check(side->indexOf(a) < 0 && fw->isVisible(), "left the slot, window shown");
        check(a->content()->maximumWidth() == QWIDGETSIZE_MAX, "width cap lifted while floating");
        check(a->titleBar()->pinButton()->isVisible() && !a->titleBar()->closeButton()->isVisible(),
              "floating: pin shown, × hidden");

        a->titleBar()->pinButton()->click();
        settle();
        check(fw->isAlwaysOnTop() && (fw->windowFlags() & Qt::WindowStaysOnTopHint),
              "pin sets always-on-top");
        fw->setGeometry(120, 140, 333, 222);
        settle();

        fw->close();                       // the window manager's ×
        settle();
        check(!a->isFloating() && side->indexOf(a) == 1, "closing the window docks it back");
        check(side->stretch(1) == 2, "same slot, same stretch");
        check(a->content()->maximumWidth() == 250, "width cap restored when docked");
        check(!mgr.floatingWindow("vfoA"), "window gone");

        b->titleBar()->closeButton()->click();
        settle();
        check(!b->isContainerVisible() && !b->isVisible(), "× hides the box");

        a->titleBar()->floatButton()->click();   // leave A floating for the restore check
        settle();
        auto* fw2 = mgr.floatingWindow("vfoA");
        check(fw2 && fw2->geometry().size() == QSize(333, 222), "re-float remembers the size");
        check(fw2 && fw2->isAlwaysOnTop(), "re-float remembers the pin");
        mgr.prepareShutdown();
    }
    settle();
    // The manager is gone: its floating window took box A with it. Rebuild
    // the boxes like a fresh start and let a new manager restore.
    {
        ContainerManager mgr(&win);
        auto* a = mgr.create("vfoA", "VFO A", new QLabel("A"));
        auto* b = mgr.create("log", "LOG", new QLabel("log"));
        auto* c = mgr.create("new", "NEW", new QLabel("new"));
        side->addWidget(a);
        side->addWidget(b);
        side->addWidget(c);
        settle();
        const int slotA = side->indexOf(a);
        mgr.restoreState();
        settle();
        check(a->isFloating() && mgr.floatingWindow("vfoA"), "restore: A floats again");
        check(!b->isContainerVisible() && !b->isVisible(), "restore: LOG still hidden");
        check(!c->isFloating() && c->isVisible(), "restore: unknown-to-state box untouched");
        mgr.setContainerVisible("log", true);
        check(b->isVisible(), "show again");
        mgr.dockContainer("vfoA");
        check(side->indexOf(a) == slotA, "restored float docks to its slot");
        const QString json = QSettings().value("panel/containers").toString();
        check(json.contains("\"vfoA\":{\"mode\":\"docked\""), "state saved as JSON");
    }

    std::printf(fails ? "\ncontainertest: %d FAILED\n" : "\ncontainertest: all ok\n", fails);
    return fails ? 1 : 0;
}
