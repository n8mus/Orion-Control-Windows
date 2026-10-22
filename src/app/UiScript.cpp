// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/UiScript.h"

#include <QAbstractButton>
#include <QApplication>
#include <QGroupBox>
#include <QAction>
#include <QFile>
#include <QMenu>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <QWidget>
#include <cstdio>

#include "ui/FrequencyDisplay.h"

namespace ttc {

UiScript::UiScript(QWidget* root, const QString& path) : QObject(root), root_(root) {
    QFile f(path);
    if (f.open(QIODevice::ReadOnly | QIODevice::Text))
        lines_ = QString::fromUtf8(f.readAll()).split('\n');
    std::fprintf(stderr, "[uiscript] %s: %d lines\n", qPrintable(path), int(lines_.size()));
    QTimer::singleShot(2000, this, &UiScript::step);   // let start-up settle
}

namespace {
template <class T>
QList<T*> visible(QWidget* root) {
    QList<T*> out;
    for (T* w : root->findChildren<T*>())
        if (w->isVisible()) out.push_back(w);
    return out;
}
}  // namespace

void UiScript::step() {
    if (next_ >= lines_.size()) {
        std::fprintf(stderr, "[uiscript] done\n");
        return;
    }
    const QString line = lines_[next_++].trimmed();
    // A step that opens a modal dialog or popup menu would sit in its nested
    // event loop forever (the selftest's quit can't end it): after 4 s, name
    // it in the log and close it, so a surprise popup is a log DIFFERENCE,
    // not a hung run.
    const int me = next_;
    QTimer::singleShot(4000, this, [this, me, line] {
        if (next_ != me) return;                   // step already returned
        QWidget* m = QApplication::activeModalWidget();
        if (!m) m = QApplication::activePopupWidget();
        if (!m) return;
        std::fprintf(stderr, "[uiscript] MODAL %s '%s' opened by '%s' -- closed\n",
                     m->metaObject()->className(), qPrintable(m->windowTitle()),
                     qPrintable(line));
        m->close();
    });
    int delay = 300;
    const QStringList a = line.split(' ', Qt::SkipEmptyParts);
    const auto text = [](const QString& s) { return QString(s).replace('_', ' '); };
    if (a.isEmpty() || a[0].startsWith('#')) {
        delay = 0;
    } else if (a[0] == "wait" && a.size() > 1) {
        delay = a[1].toInt();
    } else if (a[0] == "click" && a.size() > 1) {
        const QString t = text(a[1]);
        const int n = a.size() > 2 ? a[2].toInt() : 0;
        int seen = 0;
        QAbstractButton* hit = nullptr;
        for (QAbstractButton* b : visible<QAbstractButton>(root_))
            if (b->text() == t && seen++ == n) { hit = b; break; }
        if (hit) {
            // Say WHICH control: same-label buttons (band 6 vs ATT 6) must
            // not swap silently between runs.
            const QWidget* p = hit->parentWidget();
            const auto* gb = qobject_cast<const QGroupBox*>(p);
            std::fprintf(stderr, "[uiscript] hit '%s' %s in %s\n", qPrintable(t),
                         hit->metaObject()->className(),
                         gb ? qPrintable("group " + gb->title())
                            : p ? p->metaObject()->className() : "-");
            hit->click();
        } else {
            std::fprintf(stderr, "[uiscript] MISSING button '%s' #%d\n", qPrintable(t), n);
        }
    } else if (a[0] == "tip" && a.size() > 1) {
        const QString t = line.mid(4).trimmed();      // the rest: the exact tooltip
        QAbstractButton* hit = nullptr;
        for (QAbstractButton* b : visible<QAbstractButton>(root_))
            if (b->toolTip() == t) { hit = b; break; }
        if (hit) hit->click();
        else std::fprintf(stderr, "[uiscript] MISSING tooltip '%s'\n", qPrintable(t));
    } else if (a[0] == "menu" && a.size() > 2) {
        const QString bt = text(a[1]), at = text(a[2]);
        QAction* hit = nullptr;
        for (QToolButton* b : visible<QToolButton>(root_)) {
            if (b->text() != bt || !b->menu()) continue;
            for (QAction* act : b->menu()->findChildren<QAction*>())
                if (act->text() == at) { hit = act; break; }
            if (hit) break;
        }
        if (hit) hit->trigger();
        else std::fprintf(stderr, "[uiscript] MISSING menu '%s' > '%s'\n", qPrintable(bt), qPrintable(at));
    } else if (a[0] == "freq" && a.size() > 2) {
        const auto ds = visible<FrequencyDisplay>(root_);
        const int n = a[1].toInt();
        if (n < ds.size()) emit ds[n]->frequencyEdited(a[2].toULongLong());
        else std::fprintf(stderr, "[uiscript] MISSING frequency display #%d\n", n);
    } else if (a[0] == "slider" && a.size() > 2) {
        const auto ss = visible<QSlider>(root_);
        const int n = a[1].toInt();
        if (n < ss.size()) ss[n]->setValue(a[2].toInt());
        else std::fprintf(stderr, "[uiscript] MISSING slider #%d\n", n);
    } else if (a[0] == "shot" && a.size() > 1) {
        root_->window()->grab().save(a[1]);
    } else {
        std::fprintf(stderr, "[uiscript] bad step '%s'\n", qPrintable(line));
    }
    if (!line.isEmpty() && !line.startsWith('#'))
        std::fprintf(stderr, "[uiscript] %s\n", qPrintable(line));
    QTimer::singleShot(delay, this, &UiScript::step);
}

}  // namespace ttc
