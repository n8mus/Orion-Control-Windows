// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/SkimmerWindow.h"
#include "cw/SkimmerEngine.h"

#include <QDateTime>
#include <QHeaderView>
#include <QLabel>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

namespace ttc {

SkimmerWindow::SkimmerWindow(
    SkimmerEngine* engine,
    std::function<QChar(const QString&, qint64)> status, QWidget* parent)
    : QDialog(parent), eng_(engine), status_(std::move(status)) {
    setWindowTitle("SKIM — band map");
    resize(420, 480);
    setStyleSheet(
        "QDialog { background: #141b24; }"
        "QLabel { color: #8fa3b8; font-size: 12px; }"
        "QTableWidget { background: #0b1016; color: #c8d4e0; border: 1px"
        " solid #2a3644; font-family: monospace; font-size: 13px;"
        " gridline-color: #1c2430; }"
        "QHeaderView::section { background: #1c2430; color: #8fa3b8;"
        " border: none; padding: 3px 8px; font-weight: bold; }");
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(10, 8, 10, 8);
    lay->setSpacing(6);
    table_ = new QTableWidget(0, 4, this);
    table_->setHorizontalHeaderLabels({"kHz", "CALL", "WPM", "AGE"});
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->verticalHeader()->setVisible(false);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setColumnWidth(0, 90);
    table_->setColumnWidth(1, 120);
    table_->setColumnWidth(2, 56);
    lay->addWidget(table_, 1);
    info_ = new QLabel(this);
    lay->addWidget(info_);
    connect(table_, &QTableWidget::cellClicked, this, [this](int row, int) {
        const auto* f = table_->item(row, 0);
        const auto* c = table_->item(row, 1);
        if (!f || !c) return;
        const qint64 hz = f->data(Qt::UserRole).toLongLong();
        const QString call = c->text().trimmed();
        if (hz > 0) emit tuneTo(hz, call == "?" ? QString() : call);
    });
    tick_ = new QTimer(this);
    tick_->setInterval(2000);
    connect(tick_, &QTimer::timeout, this, &SkimmerWindow::refresh);
}

void SkimmerWindow::refresh() {
    struct Row {
        qint64 hz;
        QString call;
        int wpm;
        qint64 atSecs;                     // 0 = hunting channel
    };
    std::vector<Row> rows;
    for (const auto& s : eng_->spots())
        rows.push_back({s.hz, s.call, s.wpm, s.atSecs});
    int hunting = 0;
    for (const auto& c : eng_->channelInfo()) {
        if (!c.active) continue;
        if (!c.call.isEmpty()) continue;   // already listed via its spot
        ++hunting;
        rows.push_back({c.hz, QStringLiteral("?"), c.wpm, 0});
    }
    std::sort(rows.begin(), rows.end(),
              [](const Row& a, const Row& b) { return a.hz < b.hz; });
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    table_->setRowCount(int(rows.size()));
    for (int i = 0; i < int(rows.size()); ++i) {
        const Row& r = rows[i];
        auto* f = new QTableWidgetItem(QString::number(r.hz / 1000.0, 'f', 1));
        f->setData(Qt::UserRole, r.hz);
        auto* c = new QTableWidgetItem(r.call);
        auto* w = new QTableWidgetItem(
            r.wpm > 0 ? QString::number(r.wpm) : QStringLiteral("--"));
        auto* a = new QTableWidgetItem(
            r.atSecs > 0 ? QString("%1m").arg((now - r.atSecs) / 60)
                         : QStringLiteral("…"));
        // Same status palette as the panadapter spot labels.
        QColor col(205, 140, 255);                     // skimmer violet
        if (r.call == QLatin1String("?")) col = QColor(74, 90, 110);
        else if (status_) {
            switch (status_(r.call, r.hz).toLatin1()) {
                case 'N': col = QColor(255, 92, 70); break;
                case 'B': col = QColor(255, 190, 60); break;
                case 'W': col = QColor(130, 222, 140); break;
                case 'C': col = QColor(150, 162, 178); break;
            }
        }
        c->setForeground(col);
        f->setForeground(QColor(150, 162, 178));
        w->setForeground(QColor(150, 162, 178));
        a->setForeground(QColor(150, 162, 178));
        table_->setItem(i, 0, f);
        table_->setItem(i, 1, c);
        table_->setItem(i, 2, w);
        table_->setItem(i, 3, a);
    }
    info_->setText(QString("%1 stations · %2 channels hunting · click a "
                           "row to tune")
                       .arg(int(rows.size()) - hunting)
                       .arg(hunting));
}

void SkimmerWindow::showEvent(QShowEvent* e) {
    refresh();
    tick_->start();
    QDialog::showEvent(e);
}

void SkimmerWindow::hideEvent(QHideEvent* e) {
    tick_->stop();
    QDialog::hideEvent(e);
}

} // namespace ttc
