// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QDialog>
#include <QSet>
#include <QString>

class QCheckBox;
class QLabel;
class QTableWidget;

namespace ttc {

// The multiplier grid (MULTS on the deck): one row per mult — every CQ
// zone 1-40 for CQ WW, every country/prefix/section worked — and one
// column per band. Worked cells are green; on the band under the dial a
// row still missing there is picked out in red, which is the question a
// mult window answers mid-contest: "what do I still need HERE?"
class MultWindow : public QDialog {
    Q_OBJECT
public:
    explicit MultWindow(QWidget* parent = nullptr);
    void setData(const QString& title, const QSet<QString>& multKeys,
                 bool allCqZones, const QString& currentBand);

protected:
    void showEvent(QShowEvent* e) override;

private:
    void rebuild();
    QTableWidget* table_ = nullptr;
    QLabel* head_ = nullptr;
    QCheckBox* neededOnly_ = nullptr;
    QString title_, band_;
    QSet<QString> keys_;
    bool zones_ = false;
};

} // namespace ttc
