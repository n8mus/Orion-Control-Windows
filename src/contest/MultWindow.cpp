// SPDX-License-Identifier: GPL-2.0-or-later
#include "contest/MultWindow.h"

#include <QCheckBox>
#include <QHeaderView>
#include <QLabel>
#include <QSettings>
#include <QTableWidget>
#include <QVBoxLayout>

#include "contest/ContestEngine.h"

namespace ttc {

namespace {
const QStringList kBands = {"160M", "80M", "40M", "20M", "15M", "10M"};
QString kindName(const QString& k) {
    return k == QLatin1String("Z") ? QStringLiteral("Zone")
         : k == QLatin1String("C") ? QStringLiteral("Country")
                                   : QStringLiteral("Mult");
}
} // namespace

MultWindow::MultWindow(QWidget* parent) : QDialog(parent) {
    setWindowTitle("Multipliers");
    auto* lay = new QVBoxLayout(this);
    head_ = new QLabel(this);
    lay->addWidget(head_);
    neededOnly_ = new QCheckBox("Only what is still needed on the current band", this);
    neededOnly_->setChecked(QSettings().value("contest/multsNeededOnly", false).toBool());
    connect(neededOnly_, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue("contest/multsNeededOnly", on);
        rebuild();
    });
    lay->addWidget(neededOnly_);
    table_ = new QTableWidget(this);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->verticalHeader()->setVisible(false);
    table_->setSelectionMode(QAbstractItemView::NoSelection);
    lay->addWidget(table_, 1);
    resize(460, 620);
}

void MultWindow::setData(const QString& title, const QSet<QString>& multKeys,
                         bool allCqZones, const QString& currentBand) {
    if (title == title_ && multKeys == keys_ && allCqZones == zones_ && currentBand == band_)
        return;
    title_ = title;
    keys_ = multKeys;
    zones_ = allCqZones;
    band_ = currentBand;
    if (isVisible()) rebuild();
}

void MultWindow::showEvent(QShowEvent* e) {
    rebuild();
    QDialog::showEvent(e);
}

void MultWindow::rebuild() {
    const QList<MultGridRow> rows = multGrid(keys_, zones_);
    bool perBand = false;
    for (const MultGridRow& r : rows) if (!r.bands.isEmpty()) { perBand = true; break; }
    QStringList cols = {"Type", "Mult"};
    if (perBand) cols += kBands;
    else cols << "Worked";
    table_->clear();
    table_->setColumnCount(cols.size());
    table_->setHorizontalHeaderLabels(cols);
    QList<const MultGridRow*> shown;
    int neededHere = 0, worked = 0;
    for (const MultGridRow& r : rows) {
        const bool any = r.anyBand || !r.bands.isEmpty();
        if (any) ++worked;
        const bool needHere = perBand ? !r.bands.contains(band_) : !any;
        if (needHere) ++neededHere;
        if (neededOnly_->isChecked() && !needHere) continue;
        shown << &r;
    }
    table_->setRowCount(shown.size());
    const QColor green(46, 125, 70), red(150, 40, 40), dim(40, 44, 52);
    for (int i = 0; i < shown.size(); ++i) {
        const MultGridRow& r = *shown[i];
        auto* k = new QTableWidgetItem(kindName(r.kind));
        auto* v = new QTableWidgetItem(r.value);
        table_->setItem(i, 0, k);
        table_->setItem(i, 1, v);
        if (perBand) {
            for (int b = 0; b < kBands.size(); ++b) {
                const bool w = r.bands.contains(kBands[b]);
                auto* it = new QTableWidgetItem(w ? QStringLiteral("✓") : QString());
                it->setTextAlignment(Qt::AlignCenter);
                it->setBackground(w ? green : kBands[b] == band_ ? red : dim);
                table_->setItem(i, 2 + b, it);
            }
        } else {
            const bool w = r.anyBand;
            auto* it = new QTableWidgetItem(w ? QStringLiteral("✓") : QString());
            it->setTextAlignment(Qt::AlignCenter);
            it->setBackground(w ? green : red);
            table_->setItem(i, 2, it);
        }
    }
    table_->resizeColumnsToContents();
    head_->setText(QString("<b>%1</b> — %2 worked · %3 still needed%4")
                       .arg(title_.toHtmlEscaped()).arg(worked).arg(neededHere)
                       .arg(perBand ? " on " + band_ : QString()));
}

} // namespace ttc
