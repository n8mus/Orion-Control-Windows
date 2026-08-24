// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/SwrHistoryDialog.h"

#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace ttc {

namespace {
constexpr int kColAnt = 0, kColBand = 1, kColDate = 2, kColSwr = 3,
              kColPts = 4, kColZ = 5, kColNotes = 6;

QString bestSwrText(const PanadapterWidget::SwrRun& run) {
    double minS = 99.0;
    qint64 minF = 0;
    for (const auto& p : run.pts)
        if (p.swr < minS) { minS = p.swr; minF = p.hz; }
    if (run.pts.isEmpty()) return QString();
    return QString("%1 @ %2").arg(minS, 0, 'f', 2)
                              .arg(minF / 1e6, 0, 'f', 3);
}
}  // namespace

SwrHistoryDialog::SwrHistoryDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("SWR history");
    resize(760, 440);
    setStyleSheet(
        "QDialog { background: #141b24; }"
        "QLabel { color: #8fa3b8; font-size: 12px; }"
        "QTableWidget { background: #0b1016; color: #c8d4e0; border: 1px"
        " solid #2a3644; font-family: monospace; font-size: 12px;"
        " gridline-color: #1c2430; }"
        "QHeaderView::section { background: #1c2430; color: #8fa3b8;"
        " border: none; padding: 3px 8px; font-weight: bold; }"
        "QComboBox, QPushButton { background: #1c2430; color: #c8d4e0;"
        " border: 1px solid #2a3644; padding: 3px 8px; }");

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(10, 8, 10, 8);
    lay->setSpacing(6);

    auto* filterRow = new QHBoxLayout();
    filterRow->addWidget(new QLabel("Antenna:", this));
    antFilter_ = new QComboBox(this);
    filterRow->addWidget(antFilter_);
    filterRow->addWidget(new QLabel("Band:", this));
    bandFilter_ = new QComboBox(this);
    filterRow->addWidget(bandFilter_);
    filterRow->addStretch(1);
    lay->addLayout(filterRow);
    connect(antFilter_, &QComboBox::currentIndexChanged, this,
            &SwrHistoryDialog::rebuildTable);
    connect(bandFilter_, &QComboBox::currentIndexChanged, this,
            &SwrHistoryDialog::rebuildTable);

    table_ = new QTableWidget(0, 7, this);
    table_->setHorizontalHeaderLabels(
        {"Antenna", "Band", "Date", "Best SWR @ MHz", "Pts", "Z", "Notes"});
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->verticalHeader()->setVisible(false);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setColumnWidth(kColAnt, 140);
    table_->setColumnWidth(kColBand, 50);
    table_->setColumnWidth(kColDate, 120);
    table_->setColumnWidth(kColSwr, 120);
    table_->setColumnWidth(kColPts, 40);
    table_->setColumnWidth(kColZ, 24);
    lay->addWidget(table_, 1);

    connect(table_, &QTableWidget::itemSelectionChanged, this, [this] {
        const bool has = !table_->selectedItems().isEmpty();
        smithBtn_->setEnabled(has);
        deleteBtn_->setEnabled(has);
        clearBtn_->setEnabled(has);
    });
    connect(table_, &QTableWidget::cellDoubleClicked, this,
            [this](int row, int col) {
        if (col != kColNotes) return;
        table_->selectRow(row);
        const Sel s = selection();
        if (s.band.isEmpty()) return;
        const auto* item = table_->item(row, kColNotes);
        bool ok = false;
        const QString text = QInputDialog::getMultiLineText(
            this, "Sweep notes",
            "Conditions that could affect tuning (weather, recent work, "
            "etc.):", item ? item->data(Qt::UserRole).toString() : QString(),
            &ok);
        if (ok) emit notesEdited(s.ant, s.band, s.ts, text);
    });

    auto* btnRow = new QHBoxLayout();
    smithBtn_ = new QPushButton("Open in Smith chart", this);
    deleteBtn_ = new QPushButton("Delete", this);
    clearBtn_ = new QPushButton("Clear all for this antenna+band", this);
    smithBtn_->setEnabled(false);
    deleteBtn_->setEnabled(false);
    clearBtn_->setEnabled(false);
    btnRow->addWidget(smithBtn_);
    btnRow->addWidget(deleteBtn_);
    btnRow->addWidget(clearBtn_);
    btnRow->addStretch(1);
    lay->addLayout(btnRow);

    connect(smithBtn_, &QPushButton::clicked, this, [this] {
        const Sel s = selection();
        if (!s.band.isEmpty()) emit openSmith(s.ant, s.band);
    });
    connect(deleteBtn_, &QPushButton::clicked, this, [this] {
        const Sel s = selection();
        if (s.band.isEmpty()) return;
        if (QMessageBox::question(this, "Delete sweep",
                QString("Delete the %1 sweep on %2 m from this history?")
                    .arg(s.ant.isEmpty() ? "(unlabeled)" : s.ant, s.band))
            != QMessageBox::Yes)
            return;
        emit deleteRun(s.ant, s.band, s.ts);
    });
    connect(clearBtn_, &QPushButton::clicked, this, [this] {
        const Sel s = selection();
        if (s.band.isEmpty()) return;
        if (QMessageBox::question(this, "Clear antenna+band history",
                QString("Delete ALL saved sweeps for %1 on %2 m?")
                    .arg(s.ant.isEmpty() ? "(unlabeled)" : s.ant, s.band))
            != QMessageBox::Yes)
            return;
        emit clearBucket(s.ant, s.band);
    });

    info_ = new QLabel(this);
    lay->addWidget(info_);
}

SwrHistoryDialog::Sel SwrHistoryDialog::selection() const {
    const auto rows = table_->selectionModel()
        ? table_->selectionModel()->selectedRows()
        : QModelIndexList();
    if (rows.isEmpty()) return {};
    const auto* item = table_->item(rows.first().row(), kColAnt);
    if (!item) return {};
    return {item->data(Qt::UserRole).toString(),
            table_->item(rows.first().row(), kColBand)->text(),
            table_->item(rows.first().row(), kColDate)
                ->data(Qt::UserRole).toLongLong()};
}

void SwrHistoryDialog::setRuns(const QVector<PanadapterWidget::SwrRun>& runs) {
    runs_ = runs;
    rebuildFilters();
    rebuildTable();
}

void SwrHistoryDialog::rebuildFilters() {
    // Preserve the current selection where possible across a refresh.
    const QString curAnt = antFilter_->currentData().toString();
    const QString curBand = bandFilter_->currentData().toString();
    const bool hadFilters = antFilter_->count() > 0;

    QStringList ants, bands;
    for (const auto& r : runs_) {
        if (!ants.contains(r.ant)) ants << r.ant;
        if (!bands.contains(r.band)) bands << r.band;
    }
    std::sort(ants.begin(), ants.end());
    std::sort(bands.begin(), bands.end());

    antFilter_->blockSignals(true);
    antFilter_->clear();
    antFilter_->addItem("(all antennas)", QString());
    for (const QString& a : ants)
        antFilter_->addItem(a.isEmpty() ? "(unlabeled)" : a, a);
    if (hadFilters) {
        const int idx = antFilter_->findData(curAnt);
        if (idx >= 0) antFilter_->setCurrentIndex(idx);
    }
    antFilter_->blockSignals(false);

    bandFilter_->blockSignals(true);
    bandFilter_->clear();
    bandFilter_->addItem("(all bands)", QString());
    for (const QString& b : bands) bandFilter_->addItem(b + " m", b);
    if (hadFilters) {
        const int idx = bandFilter_->findData(curBand);
        if (idx >= 0) bandFilter_->setCurrentIndex(idx);
    }
    bandFilter_->blockSignals(false);
}

void SwrHistoryDialog::rebuildTable() {
    const QString antF = antFilter_->currentData().toString();
    const QString bandF = bandFilter_->currentData().toString();
    const bool antAll = antFilter_->currentIndex() <= 0;
    const bool bandAll = bandFilter_->currentIndex() <= 0;

    QVector<const PanadapterWidget::SwrRun*> shown;
    for (const auto& r : runs_) {
        if (!antAll && r.ant != antF) continue;
        if (!bandAll && r.band != bandF) continue;
        shown.append(&r);
    }
    std::sort(shown.begin(), shown.end(),
              [](const auto* a, const auto* b) { return a->ts > b->ts; });

    table_->setRowCount(int(shown.size()));
    for (int i = 0; i < int(shown.size()); ++i) {
        const auto& r = *shown[i];
        auto* ant = new QTableWidgetItem(r.ant.isEmpty() ? "(unlabeled)" : r.ant);
        ant->setData(Qt::UserRole, r.ant);
        auto* band = new QTableWidgetItem(r.band);
        auto* date = new QTableWidgetItem(
            QDateTime::fromSecsSinceEpoch(r.ts).toString("yyyy-MM-dd HH:mm"));
        date->setData(Qt::UserRole, qlonglong(r.ts));
        auto* swr = new QTableWidgetItem(bestSwrText(r));
        auto* pts = new QTableWidgetItem(QString::number(r.pts.size()));
        const bool hasZ = std::any_of(r.pts.cbegin(), r.pts.cend(),
            [](const auto& p) { return p.zValid; });
        auto* z = new QTableWidgetItem(hasZ ? QStringLiteral("✓")
                                             : QString());
        auto* notes = new QTableWidgetItem(r.notes);
        notes->setData(Qt::UserRole, r.notes);
        notes->setToolTip(r.notes);
        for (auto* it : {ant, band, date, swr, pts, z, notes})
            it->setForeground(QColor(200, 212, 224));
        table_->setItem(i, kColAnt, ant);
        table_->setItem(i, kColBand, band);
        table_->setItem(i, kColDate, date);
        table_->setItem(i, kColSwr, swr);
        table_->setItem(i, kColPts, pts);
        table_->setItem(i, kColZ, z);
        table_->setItem(i, kColNotes, notes);
    }
    info_->setText(QString("%1 sweep%2 — double-click Notes to edit")
                       .arg(shown.size()).arg(shown.size() == 1 ? "" : "s"));
}

} // namespace ttc
