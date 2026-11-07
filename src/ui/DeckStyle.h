// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace ttc {

// The look the decks under the pan share — the contest deck's (2026-10-18)
// and the CW deck's (2026-11-02): the sidebar boxes' dark navy (on the
// desktop's light palette it read washed out — the gray labels and the
// dupe / mult colours were drawn for a dark background). Typing is white
// in dark-blue fields, labels light blue, buttons light-blue letters that
// go white on blue when on (not bold: the F-key labels fill their
// fixed-width buttons). Widget-level sheets (dupe colours, refusal
// flashes, PRAC amber) still win over these rules.
inline constexpr const char* kDeckStyle =
    "QWidget#contestDeck, QWidget#cwDeck, QWidget#rttyDeck, QDialog#contestPane"
    " { background:#121921; }"
    "QWidget { color:#e8eef5; }"
    "QLabel { color:#c8d4e0; background:transparent; }"
    "QLineEdit, QSpinBox { background:#1c2430; color:#ffffff;"
    " border:1px solid #2a3644; border-radius:3px; padding:2px 4px;"
    " selection-background-color:#2b5c8a; }"
    "QLineEdit:focus, QSpinBox:focus { border:1px solid #5db2f0; }"
    "QPushButton { background:#1c2430; color:#8fa3b8; border:1px solid #2a3644;"
    " border-radius:3px; padding:2px 2px; }"
    "QPushButton:hover { border-color:#4a5a6e; }"
    "QPushButton:checked { background:#2b5c8a; color:#ffffff; border-color:#5db2f0; }"
    "QPushButton:disabled { color:#4a5868; border-color:#222c38; }"
    "QPushButton:checked:disabled { background:#1f3a55; color:#8fa3b8; }"
    "QPlainTextEdit, QTableWidget { background:#0e141b; color:#e8eef5;"
    " border:1px solid #2a3644; gridline-color:#2a3644;"
    " selection-background-color:#2b5c8a; }"
    "QHeaderView::section { background:#1c2430; color:#8fa3b8; border:0;"
    " border-right:1px solid #2a3644; padding:2px 4px; }"
    "QTableCornerButton::section { background:#1c2430; border:0; }"
    "QScrollBar:vertical { background:#121921; width:12px; }"
    "QScrollBar::handle:vertical { background:#2a3644; border-radius:4px; min-height:20px; }"
    "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0; }";

} // namespace ttc
