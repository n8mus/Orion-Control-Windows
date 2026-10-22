// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QObject>
#include <QStringList>

class QWidget;

namespace ttc {

// TTC_UISCRIPT=<file>: drive the window like an operator, for the Classic
// layout guard (tests/classic_guard.sh). One step per line, run 300 ms apart:
//
//   wait <ms>                 pause
//   click <text> [n]          click the n-th (0-based) visible button labelled
//                             <text> ("_" in <text> stands for a space)
//   tip <tooltip>             click the first visible button with exactly this
//                             tooltip (the rest of the line; for unlabelled
//                             cells like the routing grid)
//   menu <button> <action>    open a drop-down button's menu, trigger the item
//   freq <n> <hz>             type a frequency into the n-th frequency display
//   slider <n> <value>        set the n-th visible slider
//   shot <png>                grab the window
//   # ...                     comment
//
// Steps that find nothing print "[uiscript] MISSING ..." and carry on, so a
// moved control shows up as a transcript difference, not a hang. Never put
// MOX or TUNE in a script: the guard runs against a fake radio, but the rule
// is that no test keys a transmitter.
class UiScript : public QObject {
public:
    UiScript(QWidget* root, const QString& path);

private:
    void step();
    QWidget* root_;
    QStringList lines_;
    int next_ = 0;
};

} // namespace ttc
