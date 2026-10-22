// SPDX-License-Identifier: GPL-2.0-or-later
#include "contest/QtcDialog.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include "contest/ContestEngine.h"

namespace ttc {

QtcDialog::QtcDialog(ContestDb* db,
                     std::function<void(const QString&)> keyFn,
                     std::function<void()> stopFn, QWidget* parent)
    : QDialog(parent), db_(db), keyFn_(std::move(keyFn)),
      stopFn_(std::move(stopFn)) {
    setWindowTitle("QTC — WAE");
    auto* lay = new QVBoxLayout(this);

    auto* top = new QHBoxLayout;
    top->addWidget(new QLabel("To:", this));
    to_ = new QLineEdit(this);
    to_->setMaxLength(14);
    QFont bf = to_->font();
    bf.setPointSize(bf.pointSize() + 3);
    to_->setFont(bf);
    connect(to_, &QLineEdit::textEdited, this, [this] {
        manualTo_ = true;              // the operator took the wheel
        const int pos = to_->cursorPosition();
        const QSignalBlocker b(to_);
        to_->setText(to_->text().toUpper());
        to_->setCursorPosition(pos);
        refreshCounters();
    });
    top->addWidget(to_);
    sentTo_ = new QLabel(this);
    top->addWidget(sentTo_);
    top->addStretch(1);
    blockLbl_ = new QLabel(this);
    top->addWidget(blockLbl_);
    lay->addLayout(top);

    auto* mid = new QHBoxLayout;
    auto* loadBtn = new QPushButton("Load", this);
    connect(loadBtn, &QPushButton::clicked, this, [this] { load(); });
    mid->addWidget(loadBtn);
    mid->addWidget(new QLabel(
        "oldest unreported QSOs · min(10, 10−sent, eligible)", this));
    mid->addStretch(1);
    auto* hdrBtn = new QPushButton("Send header", this);
    connect(hdrBtn, &QPushButton::clicked, this, [this] { sendHeader(); });
    mid->addWidget(hdrBtn);
    lay->addLayout(mid);

    table_ = new QTableWidget(this);
    table_->setColumnCount(5);
    table_->setHorizontalHeaderLabels({"#", "Time", "Call", "NR", ""});
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->verticalHeader()->setVisible(false);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    lay->addWidget(table_, 1);

    auto* bot = new QHBoxLayout;
    auto* allBtn = new QPushButton("Send all", this);
    connect(allBtn, &QPushButton::clicked, this, [this] { sendAll(); });
    bot->addWidget(allBtn);
    bot->addWidget(new QLabel("Esc stops the keyer instantly", this));
    bot->addStretch(1);
    auto* cancelBtn = new QPushButton("Cancel block", this);
    connect(cancelBtn, &QPushButton::clicked, this,
            [this] { cancelBlock(); });
    bot->addWidget(cancelBtn);
    confirmBtn_ = new QPushButton("Confirm && log block", this);
    connect(confirmBtn_, &QPushButton::clicked, this,
            [this] { confirmBlock(); });
    bot->addWidget(confirmBtn_);
    lay->addLayout(bot);

    status_ = new QLabel(
        "Confirm is all-or-nothing · reported QSOs lock forever", this);
    lay->addWidget(status_);

    for (QPushButton* b : findChildren<QPushButton*>()) {
        b->setAutoDefault(false);      // Enter must never "click" a send
        b->setDefault(false);
        b->setFocusPolicy(Qt::NoFocus);
    }
    resize(560, 420);
}

void QtcDialog::openFor(qint64 contestId) {
    contestId_ = contestId;
    manualTo_ = false;
    alloc_.clear();
    table_->setRowCount(0);
    confirmBtn_->setEnabled(false);
    refreshCounters();
    show();
    raise();
    activateWindow();
}

void QtcDialog::followCall(const QString& call) {
    if (manualTo_ || !isVisible()) return;
    const QSignalBlocker b(to_);
    to_->setText(call.trimmed().toUpper());
    refreshCounters();
}

void QtcDialog::refreshCounters() {
    if (contestId_ < 0) return;
    const QString to = to_->text().trimmed();
    sentTo_->setText(to.isEmpty()
                         ? QString()
                         : QString("sent to %1: %2 of 10")
                               .arg(to)
                               .arg(db_->qtcSentTo(contestId_, to)));
    blockLbl_->setText(
        QString("next block %1").arg(db_->nextQtcBlock(contestId_)));
}

void QtcDialog::load() {
    if (contestId_ < 0) return;
    const QString to = to_->text().trimmed().toUpper();
    if (!loggableCall(to)) {
        status_->setText("type (or let the deck fill) the receiving "
                         "station first");
        return;
    }
    const QList<ContestQso> qsos = db_->qsos(contestId_);
    const QList<qint64> ids =
        allocateQtc(qsos, db_->qtcReportedQsoIds(contestId_), to,
                    db_->qtcSentTo(contestId_, to));
    alloc_.clear();
    for (qint64 id : ids)
        for (const ContestQso& q : qsos)
            if (q.id == id) { alloc_ << q; break; }
    block_ = db_->nextQtcBlock(contestId_);
    table_->setRowCount(int(alloc_.size()));
    for (int i = 0; i < alloc_.size(); ++i) {
        const ContestQso& q = alloc_[i];
        table_->setItem(i, 0, new QTableWidgetItem(QString::number(i + 1)));
        table_->setItem(i, 1, new QTableWidgetItem(
            q.tsUtc.toUTC().toString("HHmm")));
        table_->setItem(i, 2, new QTableWidgetItem(q.v.call));
        table_->setItem(i, 3, new QTableWidgetItem(q.v.serialR));
        auto* send = new QPushButton("Send", table_);
        send->setFocusPolicy(Qt::NoFocus);
        connect(send, &QPushButton::clicked, this,
                [this, i] { sendRow(i); });
        table_->setCellWidget(i, 4, send);
    }
    confirmBtn_->setEnabled(!alloc_.isEmpty());
    status_->setText(alloc_.isEmpty()
                         ? QString("nothing to send to %1 — limit reached "
                                   "or no eligible QSOs").arg(to)
                         : QString("QTC %1/%2 loaded — Send all, then "
                                   "Confirm when he acknowledges")
                               .arg(block_)
                               .arg(alloc_.size()));
}

QString QtcDialog::rowText(int i) const {
    const ContestQso& q = alloc_[i];
    return q.tsUtc.toUTC().toString("HHmm") + ' ' + q.v.call + ' '
        + q.v.serialR;
}

void QtcDialog::sendHeader() {
    if (alloc_.isEmpty() || !keyFn_) return;
    keyFn_(QString("QTC %1/%2").arg(block_).arg(alloc_.size()));
}

void QtcDialog::sendRow(int i) {
    if (i < 0 || i >= alloc_.size() || !keyFn_) return;
    keyFn_(rowText(i));
    table_->selectRow(i);
}

void QtcDialog::sendAll() {
    if (alloc_.isEmpty() || !keyFn_) return;
    sendHeader();
    for (int i = 0; i < alloc_.size(); ++i) keyFn_(rowText(i));
    status_->setText("block keyed — Confirm when he acknowledges, "
                     "Cancel if it fell apart");
}

void QtcDialog::confirmBlock() {
    if (alloc_.isEmpty()) return;
    QList<qint64> ids;
    for (const ContestQso& q : alloc_) ids << q.id;
    if (!db_->addQtcBlock(contestId_, to_->text(), block_, ids, rigHz_,
                          "CW")) {
        // Silence here once cost a contest evening of doubt — say it.
        status_->setText("DATABASE ERROR — block NOT logged, QSOs still "
                         "available");
        return;
    }
    status_->setText(QString("QTC %1/%2 to %3 logged — those QSOs are "
                             "spent")
                         .arg(block_)
                         .arg(alloc_.size())
                         .arg(to_->text().trimmed()));
    alloc_.clear();
    table_->setRowCount(0);
    confirmBtn_->setEnabled(false);
    refreshCounters();
}

void QtcDialog::cancelBlock() {
    alloc_.clear();
    table_->setRowCount(0);
    confirmBtn_->setEnabled(false);
    status_->setText("block discarded — nothing spent");
}

void QtcDialog::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape) {  // stop keying, never close
        if (stopFn_) stopFn_();
        status_->setText("keying stopped");
        return;
    }
    QDialog::keyPressEvent(e);
}

} // namespace ttc
