// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/LogbookWindow.h"

#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSqlQuery>
#include <QSqlTableModel>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include "log/LogDb.h"
#include "ui/OnlineLogsDialog.h"

namespace ttc {

namespace {
// Column order in the qso table (see LogDb's schema).
enum Col { ColId = 0, ColTs, ColCall, ColBand, ColFreq, ColMode, ColRstS,
           ColRstR, ColName, ColQth, ColGrid, ColCountry, ColCqz, ColItuz,
           ColPota, ColComment, ColQsl, ColLotw, ColEqsl };
} // namespace

LogbookWindow::LogbookWindow(LogDb* db, const CtyLookup* cty,
                             QslUploader* uploader, QWidget* parent)
    : QDialog(parent), db_(db), cty_(cty), uploader_(uploader) {
    setModal(false);
    setWindowTitle("Logbook — " +
                   QSettings().value("station/callsign", "N8EM").toString());
    resize(880, 520);
    setStyleSheet(
        "QDialog { background: #141b24; color: #dde7f0; font-size: 14px; }"
        "QLabel { color: #b8c8d8; font-size: 13px; }"
        "QLineEdit { background: #1c2430; color: #eef4e2; border: 1px solid"
        " #3a4a5e; border-radius: 3px; padding: 5px 8px; font-size: 14px; }"
        "QPushButton { background: #24303e; color: #dde7f0; border: 1px solid"
        " #3a4a5e; border-radius: 3px; padding: 6px 12px; font-size: 13px;"
        " font-weight: bold; }"
        "QPushButton:hover { border-color: #6aa5d8; }"
        "QTableView { background: #10161e; color: #c6d2df; border: 1px solid"
        " #2a3644; font-size: 13px; gridline-color: #1c2530;"
        " selection-background-color: #24406b; selection-color: #eef4f9; }"
        "QHeaderView::section { background: #1a222c; color: #8fa2b5;"
        " border: none; padding: 4px 6px; font-size: 11px; }");

    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(12, 10, 12, 10);
    v->setSpacing(8);

    auto* tools = new QHBoxLayout;
    search_ = new QLineEdit(this);
    search_->setPlaceholderText("call, park, grid, country…");
    search_->setMaximumWidth(280);
    search_->setClearButtonEnabled(true);
    tools->addWidget(search_);
    tools->addStretch(1);
    auto* delBtn = new QPushButton("Delete", this);
    delBtn->setToolTip("Delete the selected QSOs from the station log\n"
                       "(local only — nothing is retracted from online logs)");
    tools->addWidget(delBtn);
    auto* impBtn = new QPushButton("Import ADIF", this);
    tools->addWidget(impBtn);
    auto* expBtn = new QPushButton("Export ADIF", this);
    tools->addWidget(expBtn);
    auto* onlineBtn = new QPushButton("Online logs…", this);
    onlineBtn->setToolTip("LoTW / eQSL / QRZ / ClubLog / HRDLOG uploads —\n"
                          "per-service setup with Test buttons");
    tools->addWidget(onlineBtn);
    connect(onlineBtn, &QPushButton::clicked, this, [this] {
        if (!onlineDlg_) onlineDlg_ = new OnlineLogsDialog(uploader_, this);
        onlineDlg_->show();
        onlineDlg_->raise();
        onlineDlg_->activateWindow();
    });
    v->addLayout(tools);

    model_ = new QSqlTableModel(this, db_->database());
    model_->setTable("qso");
    model_->setEditStrategy(QSqlTableModel::OnFieldChange);
    model_->setSort(ColTs, Qt::DescendingOrder);
    model_->select();
    const auto head = [this](int c, const char* t) {
        model_->setHeaderData(c, Qt::Horizontal, QLatin1String(t));
    };
    head(ColTs, "UTC");
    head(ColCall, "CALL");
    head(ColBand, "BAND");
    head(ColFreq, "HZ");
    head(ColMode, "MODE");
    head(ColRstS, "SENT");
    head(ColRstR, "RCVD");
    head(ColName, "NAME");
    head(ColQth, "QTH");
    head(ColGrid, "GRID");
    head(ColCountry, "COUNTRY");
    head(ColPota, "POTA");
    head(ColComment, "COMMENT");

    view_ = new QTableView(this);
    view_->setModel(model_);
    for (int c : {ColId, ColCqz, ColItuz, ColQsl, ColLotw, ColEqsl})
        view_->setColumnHidden(c, true);
    // Upload bookkeeping columns (v2) stay hidden too.
    for (int c = ColEqsl + 1; c < model_->columnCount(); ++c)
        view_->setColumnHidden(c, true);
    view_->horizontalHeader()->setStretchLastSection(true);
    view_->verticalHeader()->setVisible(false);
    view_->setSelectionBehavior(QAbstractItemView::SelectRows);
    view_->setAlternatingRowColors(false);
    view_->setSortingEnabled(true);
    view_->sortByColumn(ColTs, Qt::DescendingOrder);
    v->addWidget(view_, 1);

    stats_ = new QLabel(this);
    stats_->setStyleSheet("QLabel { color: #7e8fa1; font-size: 12px; }");
    v->addWidget(stats_);

    connect(search_, &QLineEdit::textChanged, this,
            [this] { applyFilter(); });
    connect(impBtn, &QPushButton::clicked, this, [this] { importAdif(); });
    connect(expBtn, &QPushButton::clicked, this, [this] { exportAdif(); });
    connect(delBtn, &QPushButton::clicked, this,
            [this] { deleteSelected(); });
    // Inline edits go straight to the table (OnFieldChange); tell the rest
    // of the console (worked-before index) that the log moved.
    connect(model_, &QSqlTableModel::dataChanged, this,
            [this] { if (db_) db_->notifyExternalChange(); });

    // External changes (LOG window, imports): re-select, debounced so a
    // burst of inserts doesn't thrash the view.
    refresh_ = new QTimer(this);
    refresh_->setSingleShot(true);
    refresh_->setInterval(300);
    connect(refresh_, &QTimer::timeout, this, [this] {
        model_->select();
        refreshStats();
    });
    connect(db_, &LogDb::changed, this, [this] { refresh_->start(); });

    refreshStats();
}

void LogbookWindow::applyFilter() {
    QString t = search_->text().trimmed().toUpper();
    if (t.isEmpty()) {
        model_->setFilter(QString());
    } else {
        t.replace('\'', "''");
        const QString like = "'%" + t + "%'";
        model_->setFilter(QString("UPPER(call) LIKE %1 OR UPPER(country)"
                                  " LIKE %1 OR UPPER(pota) LIKE %1 OR"
                                  " UPPER(grid) LIKE %1").arg(like));
    }
    model_->select();
    refreshStats();
}

void LogbookWindow::refreshStats() {
    QSqlQuery q(db_->database());
    QString s;
    if (q.exec("SELECT COUNT(*),"
               " COUNT(DISTINCT CASE WHEN country<>'' THEN country END),"
               " COUNT(DISTINCT CASE WHEN country<>'' AND (qsl_rcvd='Y' OR"
               "  lotw_rcvd='Y' OR eqsl_rcvd='Y') THEN country END)"
               " FROM qso")
        && q.next()) {
        s = QString("%L1 QSOs · %2 countries worked · %3 confirmed")
                .arg(q.value(0).toLongLong())
                .arg(q.value(1).toInt())
                .arg(q.value(2).toInt());
    }
    if (model_ && !model_->filter().isEmpty())
        s += QString("   (showing %1 matches)").arg(model_->rowCount());
    stats_->setText(s);
}

void LogbookWindow::importAdif() {
    const QString fn = QFileDialog::getOpenFileName(
        this, "Import ADIF", QString(), "ADIF (*.adi *.adif);;All files (*)");
    if (fn.isEmpty()) return;
    QFile f(fn);
    if (!f.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, "Import ADIF", "Can't read " + fn);
        return;
    }
    QString err;
    const int n = db_->importAdif(f, cty_, &err);
    if (n < 0)
        QMessageBox::warning(this, "Import ADIF", err);
    else
        QMessageBox::information(this, "Import ADIF",
            QString("%1 QSOs imported (duplicates skipped).").arg(n));
}

void LogbookWindow::exportAdif() {
    const QString fn = QFileDialog::getSaveFileName(
        this, "Export ADIF", "logbook.adi", "ADIF (*.adi *.adif)");
    if (fn.isEmpty()) return;
    QFile f(fn);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, "Export ADIF", "Can't write " + fn);
        return;
    }
    db_->exportAdif(f);
}

void LogbookWindow::deleteSelected() {
    const auto rows = view_->selectionModel()->selectedRows(ColId);
    if (rows.isEmpty()) return;
    if (QMessageBox::question(this, "Delete QSOs",
            QString("Delete %1 QSO(s) from the station log?")
                .arg(rows.size()))
        != QMessageBox::Yes)
        return;
    for (const QModelIndex& ix : rows)
        db_->deleteQso(ix.data().toLongLong());
}

} // namespace ttc
