// SPDX-License-Identifier: GPL-2.0-or-later
// Shared internals for the MainWindow translation units (MainWindow.cpp,
// MainWindowTuning.cpp, MainWindowOps.cpp, MainWindowTools.cpp). Nothing
// here is API — it exists so the one giant .cpp could be split into files
// that don't collide when several features land at once.
#pragma once
#include <QMenu>
#include "radio/RadioController.h"

namespace ttc {

// Offset-LO tuning (PowerSDR if_freq): the SDR always captures this far above
// the dial so its zero-IF DC artifact can never sit on the tuned frequency.
inline constexpr int kLoOffsetHz = 60000;

inline int pbtRfSign(Mode m) {
    return (m == Mode::LSB || m == Mode::CWL) ? -1 : +1;
}

// Visual placement of the passband relative to the carrier: nominal filter
// position per mode, shifted by PBT (in RF space). The nominal audio low-cut
// offset isn't CAT-readable, so this can sit a couple hundred Hz off — harmless,
// because drags are delta-anchored to the radio's real state, never absolute.
inline void edgesFromRig(Mode m, int bw, int pbt, int& lo, int& hi) {
    // AM: the indicated bandwidth is the AUDIO width — the RF passband spans
    // BOTH sidebands, so the true width is twice the number (confirmed live:
    // AM filter values run to 9000 = 18 kHz on the air).
    const int half = (m == Mode::AM) ? bw : bw / 2;
    int centerRf;
    switch (m) {
        case Mode::USB: centerRf = +half; break;
        case Mode::LSB: centerRf = -half; break;
        default:        centerRf = 0;     break;
    }
    centerRf += pbtRfSign(m) * pbt;
    lo = centerRf - half;
    hi = centerRf + half;
}

// The Orion's per-mode filter ceiling (live-verified: AM accepts up to 9000
// — matching the front-panel knob — and silently REJECTS anything higher,
// it does not clamp; SSB/CW cap at 6000). Drag math needs the real ceiling
// or wide-AM drags peg early.
inline int bwMaxFor(Mode m) { return m == Mode::AM ? 9000 : 6000; }

// The console's dark chrome, shared by every toolbar button and dropdown
// (verbatim what the DISPLAY/SPOTS buttons have always used).
inline const char* kToolBtnStyle =
    "QToolButton { background: #1c2430; color: #c8d4e0; border: 1px solid #2a3644;"
    " border-radius: 3px; font-size: 11px; padding: 2px 8px; }"
    "QToolButton::menu-indicator { image: none; }";

inline void styleMenu(QMenu* m) {
    m->setAttribute(Qt::WA_TranslucentBackground);  // real rounded corners
    m->setStyleSheet(
        "QMenu { background-color: #141b24; color: #c8d4e0;"
        " border: 1px solid #2a3644; border-radius: 6px; padding: 6px;"
        " font-size: 13px; }"
        "QMenu::item { background: transparent; padding: 6px 16px;"
        " border-radius: 4px; }"
        "QMenu::item:selected { background: #2a3644; }"
        "QMenu::item:disabled { color: #5a6b7d; }"
        "QMenu::separator { height: 1px; background: #2a3644; margin: 6px 8px; }"
        "QMenu::indicator { width: 14px; height: 14px; }"
        "QMenu::indicator:checked { background: #3f7cb4;"
        " border: 1px solid #5db2f0; border-radius: 3px; }"
        "QMenu::indicator:unchecked { background: #1c2430;"
        " border: 1px solid #2a3644; border-radius: 3px; }");
}

} // namespace ttc
