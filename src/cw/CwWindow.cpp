// SPDX-License-Identifier: GPL-2.0-or-later
#include "cw/CwWindow.h"
#include "cw/WinKeyer.h"

#include <QCheckBox>
#include <algorithm>
#include <cmath>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
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
} // namespace

CwWindow::CwWindow(QWidget* parent) : QDialog(parent) {
    setWindowTitle("CW — WinKeyer");
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

    wk_ = new WinKeyer(this);
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
                      "%mc = your call, %c = the LOG panel callsign.\n"
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

    // Row 3: memories
    for (int i = 0; i < 4; ++i) {
        mem_[i] = new QPushButton(QString("CW%1").arg(i + 1), this);
        const QString t = QSettings()
            .value(QString("cw/mem%1").arg(i + 1), kMemDefault[i]).toString();
        mem_[i]->setToolTip(t + "\n\nclick: send    right-click: edit");
        mem_[i]->setContextMenuPolicy(Qt::CustomContextMenu);
        g->addWidget(mem_[i], 3, i);
        connect(mem_[i], &QPushButton::clicked, this, [this, i] {
            sendText(QSettings()
                .value(QString("cw/mem%1").arg(i + 1), kMemDefault[i])
                .toString());
        });
        connect(mem_[i], &QWidget::customContextMenuRequested, this,
                [this, i] { editMemory(i); });
    }

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
    g->addWidget(rx_, 6, 0, 1, 5);
    connect(rxClear, &QPushButton::clicked, rx_, &QPlainTextEdit::clear);
    connect(rxOn_, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue("cw/rxDecode", on);
        emit rxDecodeWanted(isVisible() && on);
    });

    // Row 7: status
    status_ = new QLabel(this);
    g->addWidget(status_, 7, 0, 1, 5);
    g->setRowStretch(1, 1);                // spare height: 1/3 to the entry
    g->setRowStretch(6, 2);                // ... 2/3 to the decode pane
    for (int c = 0; c < 5; ++c)            // spare width -> spread evenly
        g->setColumnStretch(c, 1);

    connect(wpm_, &QSpinBox::valueChanged, this, [this](int v) {
        wk_->setSpeed(v);
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
        wk_->stop();
        tuneBtn_->setChecked(false);
        sentView_->clear();
        prevLen_ = 0;
        line_->clear();
        updateStatus("stopped");
    });
    connect(tuneBtn_, &QPushButton::toggled, this, [this](bool on) {
        if (on) emit txImminent();
        wk_->tune(on);
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
                wk_->send(out);
                sentView_->setText((sentView_->text() + out).right(60));
            }
            line_->setText(t.mid(sp + 1));
            return;
        }
        if (!live_->isChecked()) return;
        // Stream the delta; backspace unsends if the char hasn't gone out.
        if (t.length() < prevLen_) {
            for (int i = t.length(); i < prevLen_; ++i) wk_->backspace();
        } else if (t.length() > prevLen_) {
            const QString add = substitute(t.mid(prevLen_));
            emit txImminent();
            wk_->send(add);
            sentView_->setText((sentView_->text() + add).right(60));
        }
        prevLen_ = t.length();
    });
    connect(wk_, &WinKeyer::potChanged, this, [this](int wpm) {
        const QSignalBlocker b(wpm_);
        wpm_->setValue(wpm);                 // follow the physical pot
        wk_->setSpeed(wpm);
        QSettings().setValue("cw/wpm", wpm);
        updateStatus(QString("pot -> %1 WPM").arg(wpm));
    });
    connect(wk_, &WinKeyer::breakIn, this, [this] {
        sentView_->clear();
        prevLen_ = 0;
        line_->clear();
        updateStatus("paddle break-in");
    });
    connect(wk_, &WinKeyer::busyChanged, this,
            [this](bool) { updateStatus(); });
    connect(wk_, &WinKeyer::errorOccurred, this,
            [this](const QString& e) { updateStatus(e); });

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
                    if (d[1] == '4' || d[1] == '0') { wk_->stop(); }
                    else if (d[1] == '2') {
                        const int v = QString::fromLatin1(d.mid(2)).toInt();
                        if (v >= 5 && v <= 60) {
                            const QSignalBlocker b(wpm_);
                            wpm_->setValue(v);
                            wk_->setSpeed(v);
                        }
                    }
                } else {
                    emit txImminent();
                    openKeyer();             // cqrlog may key before we show
                    wk_->send(QString::fromLatin1(d).trimmed());
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

void CwWindow::openKeyer() {
    if (wk_->isOpen()) return;
    QSettings s;
    const QString dev = s.value("cw/port",
        "/dev/serial/by-id/usb-FTDI_FT232R_USB_UART_A904QF5Z-if00-port0")
        .toString();
    if (wk_->open(dev)) {
        wk_->setPotRange(s.value("cw/potMin", 7).toInt(),
                         s.value("cw/potMax", 45).toInt());
        wk_->setSpeed(wpm_->value());
        updateStatus("keyer ready");
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
    emit rxDecodeWanted(false);
}

void CwWindow::appendRx(const QString& text) {
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
void CwWindow::resizeEvent(QResizeEvent* e) {
    QDialog::resizeEvent(e);
    QFont f = line_->font();
    f.setPixelSize(std::clamp(int(line_->height() * 0.45), 18, 44));
    line_->setFont(f);
}

void CwWindow::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape) {        // Esc = stop, never close
        wk_->stop();
        tuneBtn_->setChecked(false);
        sentView_->clear();
        prevLen_ = 0;
        line_->clear();
        updateStatus("stopped");
        return;
    }
    QDialog::keyPressEvent(e);
}

QString CwWindow::substitute(QString t) const {
    t.replace("%MC", myCall_, Qt::CaseInsensitive);
    t.replace("%C", hisCall_, Qt::CaseInsensitive);
    return t;
}

void CwWindow::sendText(const QString& t) {
    emit txImminent();
    openKeyer();
    const QString out = substitute(t).simplified();
    if (out.isEmpty()) return;
    wk_->send(out);
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
    const QString base = wk_->isOpen()
        ? QString("WinKeyer ready · %1 WPM").arg(wpm_->value())
        : QString("WinKeyer not connected (%1)").arg(wk_->lastError());
    status_->setText(s.isEmpty() ? base : base + " · " + s);
}

} // namespace ttc
