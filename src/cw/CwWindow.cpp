// SPDX-License-Identifier: GPL-2.0-or-later
#include "cw/CwWindow.h"
#include "cw/WinKeyer.h"

#include <QCheckBox>
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

    // Row 1: type-ahead line
    line_ = new QLineEdit(this);
    line_->setPlaceholderText("type CW…  Enter sends the line");
    line_->setToolTip("Buffered send: type, Enter transmits the line.\n"
                      "Live keys: every keystroke goes straight to the "
                      "keyer\n(Backspace unsends a not-yet-sent character).\n"
                      "%mc = your call, %c = the LOG panel callsign.\n"
                      "Esc or the paddle stops everything instantly.");
    g->addWidget(line_, 1, 0, 1, 4);

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
    g->addWidget(rxWpm_, 4, 1);
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
        emit rxDecodeConfigChanged(fldEng_->isChecked(), som_->isChecked(),
                                   deep_->isChecked(), atk_->currentIndex(),
                                   dcy_->currentIndex());
    };
    connect(fldEng_, &QCheckBox::toggled, this, decodeChanged);
    connect(som_, &QCheckBox::toggled, this, decodeChanged);
    connect(deep_, &QCheckBox::toggled, this, decodeChanged);
    connect(atk_, &QComboBox::currentIndexChanged, this, decodeChanged);
    connect(dcy_, &QComboBox::currentIndexChanged, this, decodeChanged);

    rx_ = new QPlainTextEdit(this);
    rx_->setReadOnly(true);
    rx_->setFixedHeight(92);               // ~4 lines at the bigger font
    rx_->setStyleSheet("QPlainTextEdit { background: #0d1218; color: "
                       "#9fe89f; border: 1px solid #3a4a5e; border-radius: "
                       "3px; font-family: monospace; font-size: 16px; }");
    g->addWidget(rx_, 6, 0, 1, 4);
    connect(rxClear, &QPushButton::clicked, rx_, &QPlainTextEdit::clear);
    connect(rxOn_, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue("cw/rxDecode", on);
        emit rxDecodeWanted(isVisible() && on);
    });

    // Row 7: status
    status_ = new QLabel(this);
    g->addWidget(status_, 7, 0, 1, 4);

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
    rxWpm_->setText(QString("%1 WPM heard").arg(wpm));
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
