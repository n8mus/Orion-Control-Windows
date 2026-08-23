// SPDX-License-Identifier: GPL-2.0-or-later
#include "cw/CwWindow.h"
#include "cw/CwKeyer.h"
#include "cw/WinKeyer.h"

#include <QCheckBox>
#include <algorithm>
#include <cmath>
#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QSlider>
#include <QHostAddress>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QTextBlock>
#include <QLabel>
#include <QMenu>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTimer>
#include <QUdpSocket>

namespace ttc {

namespace {
// Memory defaults mirror the operator's proven cqrlog CW F-keys.
const char* kMemDefault[4] = {
    "CQ CQ DE %MC %MC PSE K",
    "%C DE %MC",
    "5NN MI",
    "TU 73 E E",
};

// Callsign shape incl. portable forms (K2J/4, F/N8EM). Needs the
// digit-in-the-middle structure, so CQ/TU/73/5NN never match. Shared by
// the decode-pane double-click and the DX box's "looks like a call" tint.
const QRegularExpression& callRe() {
    static const QRegularExpression re(QStringLiteral(
        "^(?:[A-Z0-9]{1,3}/)?"
        "(?:[A-Z]{1,2}|[0-9][A-Z]|[A-Z][0-9])[0-9]{1,2}[A-Z]{1,4}"
        "(?:/[A-Z0-9]{1,4})?$"));
    return re;
}
} // namespace

CwWindow::CwWindow(QWidget* parent) : QDialog(parent) {
    setModal(false);
    // Every control sets an explicit text color: widgets with a styled
    // background otherwise keep the SYSTEM palette's text — dark-on-dark
    // on some themes (live report: "panel is very difficult to read").
    // Sizes tuned for shack distance, not laptop distance.
    setStyleSheet(
        "QDialog { background: #141b24; color: #dde7f0; font-size: 15px; }"
        "QLabel { color: #b8c8d8; font-size: 14px; }"
        "QLineEdit { background: #1c2430; color: #eef4e2; border: 1px solid"
        " #3a4a5e; border-radius: 3px; padding: 6px 9px; font-size: 18px;"
        " font-family: monospace; }"
        "QSpinBox { background: #1c2430; color: #eef4e2; border: 1px solid"
        " #3a4a5e; border-radius: 3px; padding: 4px 6px; font-size: 18px;"
        " font-weight: bold; min-width: 72px; }"
        "QCheckBox { color: #b8c8d8; font-size: 14px; }"
        "QCheckBox::indicator { width: 17px; height: 17px; }"
        "QPushButton { background: #24303e; color: #dde7f0; border: 1px solid"
        " #3a4a5e; border-radius: 3px; padding: 8px 12px; font-size: 14px;"
        " font-weight: bold; }"
        "QPushButton:hover { border-color: #6aa5d8; }"
        "QPushButton:checked { background: #8a2727; border-color: #e05d5d;"
        " color: #ffe8e8; }");

    keyer_ = new WinKeyer(this);
    setWindowTitle(QString("CW — %1").arg(keyer_->caps().name));
    auto* g = new QGridLayout(this);
    g->setContentsMargins(12, 10, 12, 10);
    g->setHorizontalSpacing(8);
    g->setVerticalSpacing(8);

    // Row 0: speed + tune + stop
    g->addWidget(new QLabel("WPM", this), 0, 0);
    wpm_ = new QSpinBox(this);
    wpm_->setRange(5, 60);
    wpm_->setValue(QSettings().value("cw/wpm", 25).toInt());
    wpm_->setToolTip("Keying speed. The WinKeyer's physical speed pot also "
                     "sets this —\nturn the knob and this box follows.");
    g->addWidget(wpm_, 0, 1);
    tuneBtn_ = new QPushButton("TUNE", this);
    tuneBtn_->setCheckable(true);
    tuneBtn_->setToolTip("Steady key-down for tuning (click again to stop)");
    g->addWidget(tuneBtn_, 0, 2);
    auto* stopBtn = new QPushButton("STOP (Esc)", this);
    stopBtn->setToolTip("Dump the keying buffer immediately — same as "
                        "touching the paddle");
    g->addWidget(stopBtn, 0, 3);
    // 0-BEAT lives here too: with this window open the type-ahead line
    // owns the keyboard, so the main window's Z shortcut never fires
    // (live report: "hitting Z and nothing happens — maybe it sleeps").
    auto* zapBtn = new QPushButton("0-BEAT", this);
    zapBtn->setToolTip("Zero-beat the strongest signal in the passband —\n"
                       "same as Z in the main window. Lands the note on\n"
                       "your sidetone pitch, then refines.");
    g->addWidget(zapBtn, 0, 4);
    connect(zapBtn, &QPushButton::clicked, this,
            [this] { emit zeroBeatRequested(); });

    // Row 1: type-ahead line
    line_ = new QLineEdit(this);
    line_->setPlaceholderText("type CW…  Enter sends the line");
    line_->setToolTip("Buffered send: type, Enter transmits the line.\n"
                      "Live keys: every keystroke goes straight to the "
                      "keyer\n(Backspace unsends a not-yet-sent character).\n"
                      "%mc = your call, %c = the DX box callsign.\n"
                      "Esc or the paddle stops everything instantly.");
    line_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    g->addWidget(line_, 1, 0, 1, 5);

    // Row 2: what went out this over + send-mode toggles. Three send
    // styles: neither box = the whole line waits for Enter; Word keys =
    // each word goes out when the space bar lands (backspace can still
    // fix the word being typed); Live keys = every keystroke immediately.
    sentView_ = new QLabel(this);
    sentView_->setStyleSheet(
        "color: #a4cf82; font-family: monospace; font-size: 15px;");
    g->addWidget(sentView_, 2, 0, 1, 2);
    word_ = new QCheckBox("Word keys", this);
    word_->setChecked(QSettings().value("cw/word", false).toBool());
    word_->setToolTip("Hold letters until the space bar — each completed "
                      "word is sent as a unit.\nBackspace fixes the word "
                      "you're typing before it goes out.\nEnter sends "
                      "whatever's left on the line.");
    g->addWidget(word_, 2, 2);
    live_ = new QCheckBox("Live keys", this);
    live_->setChecked(QSettings().value("cw/live", false).toBool());
    live_->setToolTip("Stream each keystroke to the keyer as you type "
                      "instead of\nwaiting for Enter (classic keyboard-keyer "
                      "feel)");
    g->addWidget(live_, 2, 3);
    if (live_->isChecked() && word_->isChecked())  // stale settings guard
        word_->setChecked(false);

    // Row 3: the DX call, then the memories that spend it. %c used to be
    // settable ONLY by clicking (spot, decode pane, skimmer band map or
    // waterfall) and was invisible everywhere — so when the decoder prints
    // "K8 ABC" with a gap there is nothing click-shaped to grab, and no
    // way to see what the macros are about to send. This box is that
    // missing half: every click path writes it, and it takes typing.
    auto* memRow = new QWidget(this);
    auto* memLay = new QHBoxLayout(memRow);
    memLay->setContentsMargins(0, 0, 0, 0);
    memLay->setSpacing(8);
    memLay->addWidget(new QLabel("DX", memRow));
    dxCall_ = new QLineEdit(memRow);
    dxCall_->setPlaceholderText("call");
    dxCall_->setMaximumWidth(160);
    dxCall_->setToolTip(
        "The station you're working — what %c sends.\n"
        "Filled by clicking a spot, a skimmer call, or double-clicking\n"
        "a call in the decode pane; type here when the copy is broken\n"
        "up or there's nothing to click. Enter also hands it to cqrlog's\n"
        "New QSO. Amber = doesn't look like a callsign yet (sends anyway).");
    memLay->addWidget(dxCall_);
    for (int i = 0; i < 4; ++i) {
        mem_[i] = new QPushButton(QString("CW%1").arg(i + 1), this);
        const QString t = QSettings()
            .value(QString("cw/mem%1").arg(i + 1), kMemDefault[i]).toString();
        mem_[i]->setToolTip(t + "\n\nclick: send    right-click: edit");
        mem_[i]->setContextMenuPolicy(Qt::CustomContextMenu);
        memLay->addWidget(mem_[i], 1);
        connect(mem_[i], &QPushButton::clicked, this, [this, i] {
            sendText(QSettings()
                .value(QString("cw/mem%1").arg(i + 1), kMemDefault[i])
                .toString());
        });
        connect(mem_[i], &QWidget::customContextMenuRequested, this,
                [this, i] { editMemory(i); });
    }
    g->addWidget(memRow, 3, 0, 1, 4);
    connect(dxCall_, &QLineEdit::textEdited, this, [this](const QString& t) {
        // Uppercase as it lands, and drop whitespace outright: no call has
        // a space in it, so pasting the decoder's own broken "K8 ABC"
        // yields K8ABC instead of a call the macro would key with a hole.
        QString up = t.toUpper();
        up.remove(QLatin1Char(' '));
        if (up != t) {
            const int pos = dxCall_->cursorPosition();
            dxCall_->setText(up);
            dxCall_->setCursorPosition(std::min(pos, int(up.size())));
        }
        hisCall_ = up;
        tintDxCall();
    });
    // Three ways a typed call reaches cqrlog, because there is no ONE
    // gesture the operator always makes: Enter, leaving the box (clicking
    // CW1-4 takes focus, so the normal type-then-send flow fires this),
    // and spending %c on the air. announceHisCall() de-dupes, so all
    // three firing for one call still sends a single datagram.
    connect(dxCall_, &QLineEdit::returnPressed, this, [this] {
        announceHisCall();
        line_->setFocus();               // keyboard back where CW is typed
    });
    connect(dxCall_, &QLineEdit::editingFinished, this,
            [this] { announceHisCall(); });

    // Rows 4-5: CW reader — decoded text from the SDR passband (no audio
    // cable; the decoder listens exactly where CW zap parks the carrier).
    rxOn_ = new QCheckBox("RX decode", this);
    rxOn_->setChecked(QSettings().value("cw/rxDecode", true).toBool());
    rxOn_->setToolTip("Decode the tuned CW signal from the panadapter's SDR "
                      "stream.\nBest results: click the signal (CW zap puts "
                      "the carrier dead-on);\nhandles roughly 10-40 WPM, "
                      "adapts to the sender automatically.");
    g->addWidget(rxOn_, 4, 0);
    rxWpm_ = new QLabel(this);
    rxWpm_->setStyleSheet("color: #6aa5d8;");
    // Always rich text: with AutoText the tagless placeholder string
    // ("— WPM&nbsp;·&nbsp;— Hz") is detected as PLAIN text and the
    // entities print literally — a "&nbsp;" flash every time the tone
    // drops out (live-found).
    rxWpm_->setTextFormat(Qt::RichText);
    // Reserve the widest text this label will ever show: its width must
    // NEVER change at runtime, or the stretched grid re-negotiates and
    // the whole row visibly bounces at the pitch-update rate (live
    // report: "a bounce related to the Hz readout").
    rxWpm_->setMinimumWidth(
        rxWpm_->fontMetrics().horizontalAdvance("88 WPM · 8888 Hz") + 12);
    g->addWidget(rxWpm_, 4, 1);
    radioSrc_ = new QCheckBox("RADIO src", this);
    radioSrc_->setChecked(QSettings().value("cw/rxRadio", false).toBool());
    radioSrc_->setToolTip(
        "Decode from the RADIO's audio (SignaLink) instead of the SDR.\n"
        "The radio's antenna, crystal filter and AGC feed the decoder —\n"
        "the same input fldigi gets, best for weak signals. Needs the\n"
        "AF path alive (the radio tuned on the station, sidetone 550).\n"
        "Unchecked: decode from the SDR at the dial (AF can be zero).");
    g->addWidget(radioSrc_, 4, 2);
    connect(radioSrc_, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue("cw/rxRadio", on);
        emit rxSourceChanged(on);
    });
    auto* rxClear = new QPushButton("Clear", this);
    g->addWidget(rxClear, 4, 3);
    // Row 5: decode-engine adjustments. FLD = the ported fldigi decode
    // engine (see FldigiCwEngine.h) for THIS tuned reader; SOM = fldigi's
    // fuzzy whole-character matcher; DEEP narrows the filter for weak
    // signals (adds latency); ATK/DCY are fldigi's tracker speeds.
    fldEng_ = new QCheckBox("FLD engine", this);
    fldEng_->setChecked(QSettings().value("cw/engine", true).toBool());
    fldEng_->setToolTip("Decode with the engine ported from fldigi (best "
                        "copy).\nUnchecked = the original console decoder.");
    g->addWidget(fldEng_, 5, 0);
    som_ = new QCheckBox("SOM", this);
    som_->setChecked(QSettings().value("cw/som", true).toBool());
    som_->setToolTip("Fuzzy whole-character matching: the closest valid "
                     "character wins,\nso one smeared element can't bust the "
                     "letter. fldigi's SOM decoder.");
    g->addWidget(som_, 5, 1);
    deep_ = new QCheckBox("DEEP", this);
    deep_->setChecked(QSettings().value("cw/deep", false).toBool());
    deep_->setToolTip("Weak-signal mode: much narrower filter — better SNR, "
                      "slower response.\nFor stations near the noise; leave "
                      "off for normal copy.");
    g->addWidget(deep_, 5, 2);
    auto* trk = new QWidget(this);
    auto* trkLay = new QHBoxLayout(trk);
    trkLay->setContentsMargins(0, 0, 0, 0);
    trkLay->setSpacing(4);
    nr_ = new QCheckBox("NR", trk);
    nr_->setChecked(QSettings().value("cw/nr", false).toBool());
    nr_->setToolTip("AI noise reduction (RNNoise) ahead of the decoder —\n"
                    "RADIO source only. Ruler: perfect copy to -6 dB SNR\n"
                    "where raw audio busts. ~10 ms extra latency.");
    trkLay->addWidget(nr_);
    atk_ = new QComboBox(trk);
    atk_->addItems({"ATK slow", "ATK norm", "ATK fast"});
    atk_->setCurrentIndex(QSettings().value("cw/attack", 1).toInt());
    atk_->setToolTip("Signal-tracker attack speed (fldigi's RX attack)");
    dcy_ = new QComboBox(trk);
    dcy_->addItems({"DCY slow", "DCY norm", "DCY fast"});
    dcy_->setCurrentIndex(QSettings().value("cw/decay", 1).toInt());
    dcy_->setToolTip("Signal-tracker decay speed (fldigi's RX decay)");
    trkLay->addWidget(atk_);
    trkLay->addWidget(dcy_);
    g->addWidget(trk, 5, 3);
    // Noise squelch: how strong a signal the reader needs before it prints.
    // Turn UP on a quiet band to stop stray letters into noise; DOWN to dig
    // out weak ones. Gates the fldigi engine's signal metric (0..40 here).
    // Vertical, spanning rows 3-5 of column 4 (beside CW4 / Clear / the
    // tracker combos) — the old one-cell horizontal slider was a ~60 px
    // thumb-flick, too tiny to set with any precision.
    auto* sqW = new QWidget(this);
    auto* sqLay = new QVBoxLayout(sqW);
    sqLay->setContentsMargins(0, 0, 0, 0);
    sqLay->setSpacing(2);
    auto* sqLbl = new QLabel("SQL", sqW);
    sqLbl->setAlignment(Qt::AlignHCenter);
    sqLay->addWidget(sqLbl);
    sql_ = new QSlider(Qt::Vertical, sqW);
    sql_->setRange(0, 40);
    sql_->setValue(QSettings().value("cw/squelch", 12).toInt());
    sql_->setToolTip("Noise squelch: higher = needs a stronger signal to "
                     "print,\nso a quiet band stops throwing random letters. "
                     "Lower to\ndig out weak CW. (fldigi engine)");
    sqLay->addWidget(sql_, 1, Qt::AlignHCenter);
    auto* sqVal = new QLabel(QString::number(sql_->value()), sqW);
    sqVal->setAlignment(Qt::AlignHCenter);
    sqVal->setMinimumWidth(20);
    sqLay->addWidget(sqVal);
    g->addWidget(sqW, 3, 4, 3, 1);
    connect(sql_, &QSlider::valueChanged, this, [this, sqVal](int v) {
        sqVal->setText(QString::number(v));
        QSettings().setValue("cw/squelch", v);
        emit rxSquelchChanged(v);
    });
    const auto decodeChanged = [this] {
        QSettings s;
        s.setValue("cw/engine", fldEng_->isChecked());
        s.setValue("cw/som", som_->isChecked());
        s.setValue("cw/deep", deep_->isChecked());
        s.setValue("cw/attack", atk_->currentIndex());
        s.setValue("cw/decay", dcy_->currentIndex());
        QSettings().setValue("cw/nr", nr_->isChecked());
        emit rxDecodeConfigChanged(fldEng_->isChecked(), som_->isChecked(),
                                   deep_->isChecked(), atk_->currentIndex(),
                                   dcy_->currentIndex());
        emit rxNrChanged(nr_->isChecked());
    };
    connect(fldEng_, &QCheckBox::toggled, this, decodeChanged);
    connect(som_, &QCheckBox::toggled, this, decodeChanged);
    connect(deep_, &QCheckBox::toggled, this, decodeChanged);
    connect(atk_, &QComboBox::currentIndexChanged, this, decodeChanged);
    connect(dcy_, &QComboBox::currentIndexChanged, this, decodeChanged);
    connect(nr_, &QCheckBox::toggled, this, decodeChanged);

    // Row 6: the RADIO's own CW settings, over CAT. These four are rig-
    // side — the Orion makes the sidetone and shapes the CW envelope
    // whenever it transmits CW, no matter what did the keying — so they
    // work with the WinKeyer sending, and belong beside it. The internal
    // keyer's own controls (*CK on/off, *CS speed, *CW weight) are
    // deliberately NOT here: enabling that keyer re-reads the key jack as
    // a paddle and would fight the WinKeyer wired into it. It's shown
    // read-only instead, so the operator can see what the rig thinks.
    rigBox_ = new QGroupBox("RADIO — CW (over CAT)", this);
    rigBox_->setStyleSheet(
        "QGroupBox { border: 1px solid #3a4a5e; border-radius: 4px;"
        " margin-top: 8px; padding-top: 6px; color: #b8c8d8; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 9px;"
        " padding: 0 4px; }");
    auto* rg = new QGridLayout(rigBox_);
    rg->setContentsMargins(10, 4, 10, 6);
    rg->setHorizontalSpacing(8);
    rg->setVerticalSpacing(6);
    // One throttled slider. The timer coalesces a drag: the CAT link also
    // carries the 200 ms poll rotation, and the Orion silently drops
    // commands when its parser is busy.
    auto rigSlider = [this, rg](int row, int col, const char* cap,
                                const char* tip, int lo, int hi, int snap,
                                QSlider** out, QLabel** val,
                                void (CwWindow::*sig)(int)) {
        auto* c = new QLabel(cap, rigBox_);
        c->setToolTip(tip);
        auto* s = new QSlider(Qt::Horizontal, rigBox_);
        s->setRange(lo, hi);
        s->setSingleStep(snap);
        s->setPageStep(snap * 5);
        s->setToolTip(tip);
        auto* v = new QLabel("--", rigBox_);
        v->setFixedWidth(42);
        v->setToolTip(tip);
        rg->addWidget(c, row, col * 3);
        rg->addWidget(s, row, col * 3 + 1);
        rg->addWidget(v, row, col * 3 + 2);
        rg->setColumnStretch(col * 3 + 1, 1);
        auto* t = new QTimer(this);
        t->setSingleShot(true);
        t->setInterval(60);
        connect(s, &QSlider::valueChanged, this, [s, v, t, snap, lo, hi](int nv) {
            // Snap to the radio's own resolution while dragging. The Orion
            // tunes its sidetone in 10 Hz steps like the front panel, so a
            // slider offering 553 was offering a number the rig can't take
            // (operator-found). A readback is shown as-is — display the
            // truth even when it isn't on the grid.
            if (snap > 1) {
                const int q = std::clamp(((nv + snap / 2) / snap) * snap,
                                         lo, hi);
                if (q != nv) {
                    const QSignalBlocker b(s);
                    s->setValue(q);
                    nv = q;
                }
            }
            v->setText(QString::number(nv));
            if (!t->isActive()) t->start();        // leading edge + latest
        });
        connect(t, &QTimer::timeout, this,
                [this, s, sig] { (this->*sig)(s->value()); });
        *out = s;
        *val = v;
    };
    rigSlider(0, 0, "SIDETONE",
              "How loud the radio's CW sidetone is in your ears (*CV).\n"
              "The radio's own monitor level — nothing to do with the\n"
              "WinKeyer, and it applies however the rig is keyed.",
              0, 100, 1, &rigVol_, &rigVolVal_,
              &CwWindow::rigSidetoneVolChanged);
    rigSlider(0, 1, "PITCH",
              "Sidetone pitch in Hz (*CT). The console follows this: the\n"
              "reader, zero-beat and the Hz readout all retune to whatever\n"
              "the radio says, instead of assuming 550.",
              300, 1200, 10, &rigPitch_, &rigPitchVal_,
              &CwWindow::rigSidetonePitchChanged);
    rigSlider(1, 0, "QSK",
              "Break-in delay (*CQ): how long the radio stays in transmit\n"
              "after the last element before it lets you hear again.\n"
              "Lower = hear between elements; higher = fewer relay flips.",
              0, 100, 1, &rigQsk_, &rigQskVal_,
              &CwWindow::rigQskDelayChanged);
    rigSlider(1, 1, "RISE",
              "CW envelope attack/decay in ms (*CD). Lower is crisper and\n"
              "wider on the band; higher is softer and easier on the ears.",
              3, 10, 1, &rigRise_, &rigRiseVal_,
              &CwWindow::rigAttackDecayChanged);
    rigKeyer_ = new QLabel(this);
    rigKeyer_->setStyleSheet("color: #7f93a8; font-size: 13px;");
    rigKeyer_->setToolTip(
        "The RADIO's built-in keyer, shown read-only.\n"
        "The console doesn't drive it: turning it on re-reads the key\n"
        "jack as a paddle, which would fight the WinKeyer plugged in\n"
        "there. Your sending speed is the WinKeyer's (and its pot).");
    rg->addWidget(rigKeyer_, 2, 0, 1, 6);
    g->addWidget(rigBox_, 6, 0, 1, 5);
    updateRigKeyerLine();
    rigBox_->setEnabled(false);            // until a radio says it can

    rx_ = new QPlainTextEdit(this);
    rx_->setReadOnly(true);
    // Resizing the window feeds the decode pane: extra height grows the
    // reading area, extra width stretches everything (operator request —
    // dragging bigger used to just add blank margin).
    rx_->setMinimumHeight(92);             // ~4 lines at the bigger font
    rx_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    rx_->setStyleSheet("QPlainTextEdit { background: #0d1218; color: "
                       "#9fe89f; border: 1px solid #3a4a5e; border-radius: "
                       "3px; font-family: monospace; font-size: 16px; }");
    g->addWidget(rx_, 7, 0, 1, 5);
    rx_->setToolTip("Decoded CW. Double-click a callsign to put it in the "
                    "DX box\n(the %c macro); right-click to erase.");
    rx_->viewport()->installEventFilter(this);   // double-click call capture
    connect(rxClear, &QPushButton::clicked, rx_, &QPlainTextEdit::clear);
    // Erase where the mouse already is: right-click the decode text
    // itself (in addition to the Clear button on the row above).
    rx_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(rx_, &QPlainTextEdit::customContextMenuRequested, this,
            [this](const QPoint& pos) {
        QMenu menu(rx_);
        menu.setStyleSheet(
            "QMenu { background: #1c2430; color: #dde7f0; border: 1px solid"
            " #3a4a5e; } QMenu::item:selected { background: #2a3644; }");
        QAction* erase = menu.addAction("Erase all");
        QAction* copy = menu.addAction("Copy");
        copy->setEnabled(rx_->textCursor().hasSelection());
        QAction* selAll = menu.addAction("Select all");
        QAction* act = menu.exec(rx_->mapToGlobal(pos));
        if (act == erase) rx_->clear();
        else if (act == copy) rx_->copy();
        else if (act == selAll) rx_->selectAll();
    });
    connect(rxOn_, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue("cw/rxDecode", on);
        emit rxDecodeWanted(isVisible() && on);
    });

    // Row 7: status
    status_ = new QLabel(this);
    g->addWidget(status_, 8, 0, 1, 5);
    g->setRowStretch(1, 1);                // spare height: 1/3 to the entry
    g->setRowStretch(7, 2);                // ... 2/3 to the decode pane
    for (int c = 0; c < 5; ++c)            // spare width -> spread evenly
        g->setColumnStretch(c, 1);

    connect(wpm_, &QSpinBox::valueChanged, this, [this](int v) {
        keyer_->setSpeed(v);
        QSettings().setValue("cw/wpm", v);
        updateStatus();
    });
    connect(live_, &QCheckBox::toggled, this, [this](bool on) {
        if (on) word_->setChecked(false);  // one send style at a time
        QSettings().setValue("cw/live", on);
        prevLen_ = 0;
        line_->clear();
    });
    connect(word_, &QCheckBox::toggled, this, [this](bool on) {
        if (on) live_->setChecked(false);
        QSettings().setValue("cw/word", on);
        prevLen_ = 0;
        line_->clear();
    });
    connect(stopBtn, &QPushButton::clicked, this, [this] {
        keyer_->stop();
        tuneBtn_->setChecked(false);
        sentView_->clear();
        prevLen_ = 0;
        line_->clear();
        updateStatus("stopped");
    });
    connect(tuneBtn_, &QPushButton::toggled, this, [this](bool on) {
        if (on) emit txImminent();
        keyer_->tune(on);
        updateStatus(on ? "TUNE — key down" : QString());
    });
    connect(line_, &QLineEdit::returnPressed, this, [this] {
        if (live_->isChecked()) { prevLen_ = 0; line_->clear(); return; }
        sendText(line_->text());
        line_->clear();
    });
    connect(line_, &QLineEdit::textEdited, this, [this](const QString& t) {
        // Word keys: completed words (everything up to the last space)
        // leave for the keyer; the word still being typed stays editable
        // in the line. The trailing space rides along = the word gap.
        if (word_->isChecked()) {
            const int sp = t.lastIndexOf(' ');
            if (sp < 0) return;
            const QString out = substitute(t.left(sp + 1));
            if (!out.simplified().isEmpty()) {
                emit txImminent();
                openKeyer();
                keyer_->send(out);
                sentView_->setText((sentView_->text() + out).right(60));
            }
            line_->setText(t.mid(sp + 1));
            return;
        }
        if (!live_->isChecked()) return;
        // Stream the delta; backspace unsends if the char hasn't gone out.
        if (t.length() < prevLen_) {
            for (int i = t.length(); i < prevLen_; ++i) keyer_->backspace();
        } else if (t.length() > prevLen_) {
            const QString add = substitute(t.mid(prevLen_));
            emit txImminent();
            keyer_->send(add);
            sentView_->setText((sentView_->text() + add).right(60));
        }
        prevLen_ = t.length();
    });
    connect(keyer_, &CwKeyer::potChanged, this, [this](int wpm) {
        const QSignalBlocker b(wpm_);
        wpm_->setValue(wpm);                 // follow the physical pot
        keyer_->setSpeed(wpm);
        QSettings().setValue("cw/wpm", wpm);
        updateStatus(QString("pot -> %1 WPM").arg(wpm));
    });
    connect(keyer_, &CwKeyer::breakIn, this, [this] {
        sentView_->clear();
        prevLen_ = 0;
        line_->clear();
        updateStatus("paddle break-in");
    });
    connect(keyer_, &CwKeyer::busyChanged, this,
            [this](bool) { updateStatus(); });
    connect(keyer_, &CwKeyer::errorOccurred, this,
            [this](const QString& e) { updateStatus(e); });

    // Decode-text feed: every decoded chunk is also datagrammed to
    // localhost so a contest logger (Not1MM's CW-decode dock) can mirror
    // the readout inside its own window — no window hopping, no focus
    // steal, console free to sit minimized. Send-only, no listener needed.
    feed_ = new QUdpSocket(this);
    feedPort_ = quint16(QSettings().value("cw/feedPort", 2336).toInt());

    // cwdaemon-protocol server: cqrlog (CW interface set to cwdaemon,
    // localhost:6789) keys through us; the one WinKeyer serves both.
    daemon_ = new QUdpSocket(this);
    const quint16 dport =
        quint16(QSettings().value("cw/daemonPort", 6789).toInt());
    if (daemon_->bind(QHostAddress::LocalHost, dport)) {
        connect(daemon_, &QUdpSocket::readyRead, this, [this] {
            while (daemon_->hasPendingDatagrams()) {
                QByteArray d(int(daemon_->pendingDatagramSize()), 0);
                daemon_->readDatagram(d.data(), d.size());
                if (d.startsWith('\x1b')) {
                    if (d.size() < 2) continue;
                    if (d[1] == '4' || d[1] == '0') { keyer_->stop(); }
                    else if (d[1] == '2') {
                        const int v = QString::fromLatin1(d.mid(2)).toInt();
                        if (v >= 5 && v <= 60) {
                            const QSignalBlocker b(wpm_);
                            wpm_->setValue(v);
                            keyer_->setSpeed(v);
                        }
                    }
                } else {
                    emit txImminent();
                    openKeyer();             // cqrlog may key before we show
                    keyer_->send(QString::fromLatin1(d).trimmed());
                }
            }
        });
    }

    // QDialog makes push buttons auto-default, so Enter in the type-ahead
    // line ALSO "clicked" the dialog's default button — the typed text
    // went out with a macro right behind it (live-found on the air).
    for (QPushButton* b : findChildren<QPushButton*>()) {
        b->setAutoDefault(false);
        b->setDefault(false);
    }
}

bool CwWindow::keyerOpen() const { return keyer_->isOpen(); }

void CwWindow::openKeyer() {
    if (keyer_->isOpen()) return;
    if (keyer_->open()) {
        keyer_->setSpeed(wpm_->value());
        updateStatus("keyer ready");
    } else {
        updateStatus();          // the base line already carries lastError()
    }
}

void CwWindow::showEvent(QShowEvent* e) {
    QDialog::showEvent(e);
    openKeyer();
    line_->setFocus();
    updateStatus();
    emit rxDecodeWanted(rxOn_->isChecked());
}

void CwWindow::hideEvent(QHideEvent* e) {
    QDialog::hideEvent(e);
    // Spontaneous hides come from the window system — i.e. the console
    // being minimized takes this child dialog with it. Keep the decoder
    // running through those so the Not1MM decode dock stays live; only a
    // real close (programmatic hide) stops it.
    if (!e->spontaneous())
        emit rxDecodeWanted(false);
}

void CwWindow::appendRx(const QString& text) {
    if (feed_)                            // mirror to the contest logger
        feed_->writeDatagram(text.toUtf8(), QHostAddress::LocalHost,
                             feedPort_);
    rx_->moveCursor(QTextCursor::End);
    rx_->insertPlainText(text);
    rx_->moveCursor(QTextCursor::End);
    // keep it bounded on long monitoring sessions
    if (rx_->document()->characterCount() > 4000) {
        QTextCursor c(rx_->document());
        c.setPosition(0);
        c.setPosition(1000, QTextCursor::KeepAnchor);
        c.removeSelectedText();
        rx_->moveCursor(QTextCursor::End);
    }
}

void CwWindow::setRxWpm(int wpm) {
    rxWpmVal_ = wpm;
    updateRxInfo();
}

void CwWindow::setRxPitch(double hz) {
    rxPitchVal_ = hz;
    updateRxInfo();
}

// "18 WPM · 547 Hz" — the Hz is the RADIO's actual audio tone (measured
// like fldigi does), GREEN when within +/-10 Hz of the operator's pitch
// (cw/pitchHz, 550) and amber when off: on-the-note at a glance instead
// of consulting fldigi's waterfall (operator's spec). Both slots always
// render (em-dash placeholders) so the text width stays constant, and
// the shown Hz only moves when the measurement really moved (>1.5 Hz)
// — no last-digit flicker.
void CwWindow::updateRxInfo() {
    static double shownHz = -1.0;
    if (rxPitchVal_ < 0) shownHz = -1.0;
    else if (shownHz < 0 || std::abs(rxPitchVal_ - shownHz) > 1.5)
        shownHz = rxPitchVal_;
    const QString wpmPart =
        rxWpmVal_ > 0 ? QString("%1 WPM").arg(rxWpmVal_)
                      : QStringLiteral("— WPM");
    QString hzPart = QStringLiteral("— Hz");
    if (shownHz > 0) {
        const int target = QSettings().value("cw/pitchHz", 550).toInt();
        const bool on = std::abs(shownHz - target) <= 10.0;
        hzPart = QString("<span style='color:%1'>%2 Hz</span>")
                     .arg(on ? "#8fd48f" : "#e0b060")
                     .arg(qRound(shownHz));
    }
    rxWpm_->setText(wpmPart + "&nbsp;·&nbsp;" + hzPart);
}

// The entry line grows with the window — and its FONT grows with it, so
// a big window means CW you can read from across the shack.
// The fldigi gesture: double-click a decoded token; if it's shaped like a
// callsign it goes to the owner (DX box + %c macro). Token = the
// whitespace-delimited run around the click — Qt's own word selection
// would split "W1AW/4" at the slash and "N8EM" survives only by luck of
// the word-character table, so we cut the token ourselves.
bool CwWindow::eventFilter(QObject* obj, QEvent* ev) {
    if (rx_ && obj == rx_->viewport()
        && ev->type() == QEvent::MouseButtonDblClick) {
        auto* me = static_cast<QMouseEvent*>(ev);
        const QTextCursor c = rx_->cursorForPosition(me->pos());
        const QString line = c.block().text();
        int i = std::min(c.positionInBlock(), int(line.size()) - 1);
        if (line.isEmpty() || i < 0) return true;
        int a = i, b = i;
        while (a > 0 && !line[a - 1].isSpace()) --a;
        while (b < int(line.size()) && !line[b].isSpace()) ++b;
        QString tok = line.mid(a, b - a).toUpper();
        tok.remove(QRegularExpression(QStringLiteral("[^A-Z0-9/]")));
        if (callRe().match(tok).hasMatch()) {
            QTextCursor sel = c;               // show what was grabbed
            sel.setPosition(c.block().position() + a);
            sel.setPosition(c.block().position() + b, QTextCursor::KeepAnchor);
            rx_->setTextCursor(sel);
            emit callDoubleClicked(tok);
        }
        return true;                           // suppress Qt word-select
    }
    return QDialog::eventFilter(obj, ev);
}

void CwWindow::resizeEvent(QResizeEvent* e) {
    QDialog::resizeEvent(e);
    QFont f = line_->font();
    f.setPixelSize(std::clamp(int(line_->height() * 0.45), 18, 44));
    line_->setFont(f);
}

void CwWindow::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape) {        // Esc = stop, never close
        keyer_->stop();
        tuneBtn_->setChecked(false);
        sentView_->clear();
        prevLen_ = 0;
        line_->clear();
        updateStatus("stopped");
        return;
    }
    QDialog::keyPressEvent(e);
}

// Radio -> UI. Block signals: a poll answer must move the slider without
// bouncing a set straight back down the CAT link (and fighting a drag).
static void showRig(QSlider* s, QLabel* v, int val) {
    if (!s) return;
    const QSignalBlocker b(s);
    s->setValue(val);
    v->setText(QString::number(val));
}

void CwWindow::showRigSidetoneVol(int pct) { showRig(rigVol_, rigVolVal_, pct); }
void CwWindow::showRigQskDelay(int val)    { showRig(rigQsk_, rigQskVal_, val); }
void CwWindow::showRigAttackDecay(int ms)  { showRig(rigRise_, rigRiseVal_, ms); }

void CwWindow::showRigSidetonePitch(int hz) {
    showRig(rigPitch_, rigPitchVal_, hz);
}

void CwWindow::showRigKeyerSpeed(int wpm)  { rigKeyerWpm_ = wpm; updateRigKeyerLine(); }
void CwWindow::showRigKeyerWeight(int pct) { rigKeyerWt_ = pct;  updateRigKeyerLine(); }
void CwWindow::showRigKeyerEnabled(bool on){ rigKeyerOn_ = on;   updateRigKeyerLine(); }

void CwWindow::updateRigKeyerLine() {
    if (!rigKeyer_) return;
    if (rigKeyerOn_ < 0) { rigKeyer_->setText("rig keyer: —"); return; }
    QString t = QString("rig keyer: %1").arg(rigKeyerOn_ ? "ON" : "off");
    if (rigKeyerWpm_ > 0) t += QString(" · %1 wpm").arg(rigKeyerWpm_);
    if (rigKeyerWt_ > 0)  t += QString(" · wt %1").arg(rigKeyerWt_);
    // ON while a WinKeyer is wired into the key jack is worth flagging:
    // the radio then reads that jack as a paddle, so a key-down looks
    // like a held dit.
    if (rigKeyerOn_ > 0 && keyer_->isOpen())
        t += "   ⚠ paddle-mode jack — WinKeyer may key continuous dits";
    rigKeyer_->setText(t);
}

void CwWindow::setRigCwAvailable(bool on) {
    if (rigBox_) rigBox_->setEnabled(on);
}

void CwWindow::setHisCall(const QString& call) {
    hisCall_ = call.trimmed().toUpper();
    // A call that arrived by CLICK was already handed to cqrlog by the
    // owner of that click — mark it pushed so spending %c doesn't send a
    // second lookup for the same station.
    pushedCall_ = hisCall_;
    if (!dxCall_) return;
    dxCall_->setText(hisCall_);          // textEdited only fires on typing
    tintDxCall();
}

void CwWindow::announceHisCall() {
    if (hisCall_.isEmpty() || hisCall_ == pushedCall_) return;
    pushedCall_ = hisCall_;
    emit hisCallEntered(hisCall_);
}

void CwWindow::tintDxCall() {
    // Amber while it isn't call-shaped — a hint, never a block: the
    // operator gets to send whatever he means to send.
    const bool ok = hisCall_.isEmpty() || callRe().match(hisCall_).hasMatch();
    dxCall_->setStyleSheet(ok ? QString()   // fall back to the dialog sheet
                              : QStringLiteral("color: #e0b45d;"));
}

QString CwWindow::substitute(QString t) const {
    t.replace("%MC", myCall_, Qt::CaseInsensitive);
    t.replace("%C", hisCall_, Qt::CaseInsensitive);
    return t;
}

void CwWindow::sendText(const QString& t) {
    // %c with nothing armed used to key the exchange with a hole in it
    // (CW2 became " DE N8EM"). Refuse and say so — the DX box is right
    // there and now shows exactly what's missing.
    if (hisCall_.isEmpty() && t.contains(QStringLiteral("%C"),
                                         Qt::CaseInsensitive)) {
        updateStatus("no DX call — fill the DX box");
        dxCall_->setFocus();
        return;
    }
    // Sending his call ON THE AIR is the moment the QSO is real — make
    // sure cqrlog has it even if the box never lost focus (keyboard-only
    // operating: type the call, hit the macro key, never touch the mouse).
    if (t.contains(QStringLiteral("%C"), Qt::CaseInsensitive))
        announceHisCall();
    emit txImminent();
    openKeyer();
    const QString out = substitute(t).simplified();
    if (out.isEmpty()) return;
    keyer_->send(out);
    sentView_->setText(out.right(60));
}

void CwWindow::editMemory(int i) {
    const QString key = QString("cw/mem%1").arg(i + 1);
    bool ok = false;
    const QString cur =
        QSettings().value(key, kMemDefault[i]).toString();
    const QString t = QInputDialog::getText(
        this, QString("Edit CW%1").arg(i + 1),
        "Macro text (%mc = my call, %c = his call):",
        QLineEdit::Normal, cur, &ok);
    if (!ok) return;
    QSettings().setValue(key, t);
    mem_[i]->setToolTip(t + "\n\nclick: send    right-click: edit");
}

void CwWindow::updateStatus(const QString& s) {
    const QString base = keyer_->isOpen()
        ? QString("%1 ready · %2 WPM").arg(keyer_->caps().name)
              .arg(wpm_->value())
        : QString("%1 not connected (%2)").arg(keyer_->caps().name)
              .arg(keyer_->lastError());
    status_->setText(s.isEmpty() ? base : base + " · " + s);
}

} // namespace ttc
