// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/DigiWindow.h"
#include "net/FldigiClient.h"

#include <QCheckBox>
#include <QGridLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>

namespace ttc {

DigiWindow::DigiWindow(FldigiClient* client, QWidget* parent)
    : QDialog(parent), fl_(client) {
    setWindowTitle("DIGI — fldigi link");
    setStyleSheet(
        "QDialog { background: #141b24; }"
        "QLabel { color: #8fa3b8; font-size: 12px; font-weight: bold; }"
        "QCheckBox { color: #c8d4e0; font-size: 12px; }"
        "QSpinBox { background: #1c2430; color: #c8d4e0; border: 1px solid"
        " #2a3644; border-radius: 3px; padding: 2px 6px; }"
        "QPlainTextEdit { background: #0b1016; color: #7fd4ff; border: 1px"
        " solid #2a3644; border-radius: 3px; font-family: monospace;"
        " font-size: 13px; }");
    auto* g = new QGridLayout(this);
    g->setContentsMargins(12, 10, 12, 10);
    g->setHorizontalSpacing(10);
    g->setVerticalSpacing(8);

    status_ = new QLabel("fldigi: not connected — start fldigi "
                         "(XML-RPC on :7362)", this);
    g->addWidget(status_, 0, 0, 1, 3);

    g->addWidget(new QLabel("CARRIER", this), 1, 0);
    carrier_ = new QSpinBox(this);
    carrier_->setRange(0, 4000);
    carrier_->setSuffix(" Hz");
    carrier_->setToolTip("fldigi's audio carrier — edit to move it, or "
                         "click a trace in the passband");
    g->addWidget(carrier_, 1, 1);
    track_ = new QCheckBox("Clicks in the passband set the carrier "
                           "(radio in DIG)", this);
    track_->setChecked(
        QSettings().value("digi/trackClicks", true).toBool());
    track_->setToolTip(
        "With the radio in DIG, clicking a signal INSIDE the passband "
        "moves fldigi's\naudio cursor onto it instead of retuning the "
        "radio — the panadapter becomes\nfldigi's waterfall. Clicks "
        "outside the passband still tune normally.");
    g->addWidget(track_, 1, 2);
    connect(track_, &QCheckBox::toggled, this, [](bool on) {
        QSettings().setValue("digi/trackClicks", on);
    });

    rx_ = new QPlainTextEdit(this);
    rx_->setReadOnly(true);
    rx_->setMinimumSize(460, 180);
    g->addWidget(rx_, 2, 0, 1, 3);
    g->setColumnStretch(2, 1);
    g->setRowStretch(2, 1);

    connect(fl_, &FldigiClient::statusChanged, this,
            [this](bool on, const QString& modem, int carrierHz) {
                status_->setText(
                    on ? QString("fldigi: %1 @ %2 Hz").arg(modem)
                             .arg(carrierHz)
                       : QString("fldigi: not connected — start fldigi "
                                 "(XML-RPC on :7362)"));
                if (on && !carrier_->hasFocus()) {
                    const QSignalBlocker b(carrier_);
                    carrier_->setValue(carrierHz);
                }
            });
    connect(fl_, &FldigiClient::rxText, this, [this](const QString& t) {
        rx_->moveCursor(QTextCursor::End);
        rx_->insertPlainText(t);
        if (rx_->toPlainText().size() > 8000) {   // bound the backlog
            QTextCursor c = rx_->textCursor();
            c.movePosition(QTextCursor::Start);
            c.movePosition(QTextCursor::Down, QTextCursor::KeepAnchor, 20);
            c.removeSelectedText();
        }
        rx_->moveCursor(QTextCursor::End);
    });
    connect(carrier_, &QSpinBox::editingFinished, this, [this] {
        fl_->setCarrier(carrier_->value());
    });
}

bool DigiWindow::trackClicks() const { return track_->isChecked(); }

void DigiWindow::showEvent(QShowEvent* e) {
    fl_->setActive(true);
    QDialog::showEvent(e);
}

void DigiWindow::hideEvent(QHideEvent* e) {
    fl_->setActive(false);
    QDialog::hideEvent(e);
}

} // namespace ttc
