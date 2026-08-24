// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ui/PanadapterWidget.h"  // SwrRun

#include <QDialog>
#include <QVector>

class QTableWidget;
class QComboBox;
class QLabel;
class QPushButton;

namespace ttc {

// Every saved SWR sweep, across every antenna and band, browsable and
// prunable by hand — the one view that lines up date, best SWR, and the
// operator's own notes on conditions (wind, ice, wet ground) so an odd
// curve from months ago has its explanation right next to it.
//
// Owns no data: the caller hands in a flat snapshot via setRuns() and
// reacts to the signals below to mutate the real store, then calls
// setRuns() again to refresh. Keeps this dialog decoupled from
// MainWindow's swrRuns_ hash, same spirit as SkimmerWindow's tuneTo.
class SwrHistoryDialog : public QDialog {
    Q_OBJECT
public:
    explicit SwrHistoryDialog(QWidget* parent = nullptr);

    void setRuns(const QVector<PanadapterWidget::SwrRun>& runs);

signals:
    void openSmith(const QString& ant, const QString& band);
    void deleteRun(const QString& ant, const QString& band, qint64 ts);
    void clearBucket(const QString& ant, const QString& band);
    void notesEdited(const QString& ant, const QString& band, qint64 ts,
                      const QString& notes);

private:
    void rebuildFilters();
    void rebuildTable();
    // (ant, band, ts) of the selected row, or empty ant+band+0 if none.
    struct Sel { QString ant, band; qint64 ts = 0; };
    Sel selection() const;

    QVector<PanadapterWidget::SwrRun> runs_;
    QTableWidget* table_ = nullptr;
    QComboBox*    antFilter_  = nullptr;
    QComboBox*    bandFilter_ = nullptr;
    QLabel*       info_ = nullptr;
    QPushButton*  smithBtn_ = nullptr;
    QPushButton*  deleteBtn_ = nullptr;
    QPushButton*  clearBtn_ = nullptr;
};

} // namespace ttc
