// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/CopyAssistWindow.h"

#include <QCheckBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTextBrowser>
#include <QUrl>
#include <QVBoxLayout>

namespace ttc {

namespace {
const char* kStyle =
    "QDialog { background: #141b24; }"
    "QLabel { color: #8fa3b8; font-size: 12px; }"
    "QCheckBox { color: #e8b04a; font-weight: bold; }"
    "QSpinBox { background: #1c2430; color: #c8d4e0; }"
    "QPushButton { background: #1c2430; color: #c8d4e0; border: 1px solid #2a3644;"
    " padding: 3px 8px; }"
    "QTextBrowser { background: #0b1016; color: #c8d4e0; border: 1px solid #2a3644;"
    " font-size: 13px; }";

QString confColor(float c) {
    return c >= 0.85f ? "#00ff88" : c >= 0.65f ? "#e0e040" : c >= 0.45f ? "#ff9020" : "#ff4040";
}
// The band map's worked-before colours (PanadapterWidget).
QString callColor(char st) {
    switch (st) {
    case 'N': return "#ff5c46";                    // new one
    case 'B': return "#ffbe3c";                    // new band
    case 'W': return "#82de8c";                    // worked
    case 'C': return "#96a2b2";                    // confirmed
    default:  return "#ffd832";                    // unknown: DX yellow
    }
}
}  // namespace

CopyAssistWindow::CopyAssistWindow(bool on, int threads, QWidget* parent) : QDialog(parent) {
    setWindowTitle("Copy Assist — speech to text");
    setStyleSheet(kStyle);
    resize(640, 420);
    auto* lay = new QVBoxLayout(this);
    auto* top = new QHBoxLayout;
    on_ = new QCheckBox("Transcribe the SDR receive audio", this);
    on_->setChecked(on);
    on_->setToolTip("whisper.cpp (base.en) on this PC — nothing leaves the station.\n"
                    "Needs SDR ▾ → Receive audio on (its MUTE is fine: Copy Assist\n"
                    "listens before it). Calls spotted near the dial steer the spelling.");
    top->addWidget(on_);
    top->addStretch(1);
    top->addWidget(new QLabel("CPU threads", this));
    threads_ = new QSpinBox(this);
    threads_->setRange(2, 16);
    threads_->setValue(threads);
    threads_->setToolTip("8 keeps up ~2x faster than real time on this Xeon;\n"
                         "the skimmer bank wants the rest");
    top->addWidget(threads_);
    lay->addLayout(top);
    text_ = new QTextBrowser(this);
    text_->setOpenLinks(false);
    lay->addWidget(text_, 1);
    auto* bot = new QHBoxLayout;
    status_ = new QLabel(this);
    status_->setWordWrap(true);
    bot->addWidget(status_, 1);
    last_ = new QLabel(this);
    bot->addWidget(last_);
    auto* clear = new QPushButton("Clear", this);
    bot->addWidget(clear);
    auto* close = new QPushButton("Close", this);
    bot->addWidget(close);
    lay->addLayout(bot);

    connect(on_, &QCheckBox::toggled, this, [this](bool v) { emit enableChanged(v, threads_->value()); });
    connect(threads_, &QSpinBox::valueChanged, this, [this](int t) {
        if (on_->isChecked()) emit enableChanged(true, t);
    });
    connect(clear, &QPushButton::clicked, text_, &QTextBrowser::clear);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    connect(text_, &QTextBrowser::anchorClicked, this, [this](const QUrl& u) {
        if (u.scheme() == QLatin1String("call")) emit callClicked(u.path());
    });
}

void CopyAssistWindow::setOn(bool on) {
    const QSignalBlocker b(on_);
    on_->setChecked(on);
}

void CopyAssistWindow::setStatus(const QString& s) { status_->setText(s); }

void CopyAssistWindow::addResult(const AsrEngine::Result& r) {
    // Text with each call made a link, escaped piecewise.
    QString body;
    int pos = 0;
    for (const CallHit& h : r.calls) {
        if (h.start < pos) continue;
        body += r.text.mid(pos, h.start - pos).toHtmlEscaped();
        const char st = statusFor_ ? statusFor_(h.call, r.dialHz) : '?';
        body += QString("<a href=\"call:%1\" style=\"color:%2; font-weight:bold;"
                        " text-decoration:none\">%3</a>")
                    .arg(h.call, callColor(st), r.text.mid(h.start, h.len).toHtmlEscaped());
        if (r.text.mid(h.start, h.len).toUpper() != h.call)
            body += QString("<span style=\"color:%1\"> [%2]</span>").arg(callColor(st), h.call);
        pos = h.start + h.len;
    }
    body += r.text.mid(pos).toHtmlEscaped();
    const QString when = QDateTime::fromMSecsSinceEpoch(r.atMs).toUTC().toString("HH:mm:ss");
    const QString line =
        QString("<div><span style=\"color:#5c6b7a\">%1z %2</span> "
                "<span style=\"color:%3\">%4</span></div>")
            .arg(when)
            .arg(r.dialHz > 0 ? QString::number(r.dialHz / 1000.0, 'f', 1) : QString())
            .arg(confColor(r.confidence), body);
    QScrollBar* sb = text_->verticalScrollBar();
    const bool atEnd = sb->value() >= sb->maximum() - 4;
    text_->append(line);
    if (atEnd) sb->setValue(sb->maximum());
    last_->setText(QString("last: %1 s of speech in %2 s, confidence %3")
                  .arg(r.seconds, 0, 'f', 1).arg(r.decodeMs / 1000.0, 0, 'f', 1)
                  .arg(r.confidence, 0, 'f', 2));
}

} // namespace ttc
