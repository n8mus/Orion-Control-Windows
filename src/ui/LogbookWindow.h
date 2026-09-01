// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QDialog>

class QLabel;
class QLineEdit;
class QSqlTableModel;
class QTableView;
class QTimer;

namespace ttc {

class CtyLookup;
class LogDb;

// The station log as a table: search, inline edit, import/export ADIF.
// cqrlog on the Linux box stays the award engine — this is the daily view.
class LogbookWindow : public QDialog {
    Q_OBJECT
public:
    LogbookWindow(LogDb* db, const CtyLookup* cty, QWidget* parent = nullptr);

private:
    void applyFilter();
    void refreshStats();
    void importAdif();
    void exportAdif();
    void deleteSelected();

    LogDb* db_;
    const CtyLookup* cty_;
    QSqlTableModel* model_ = nullptr;
    QTableView* view_ = nullptr;
    QLineEdit* search_ = nullptr;
    QLabel* stats_ = nullptr;
    QTimer* refresh_ = nullptr;
};

} // namespace ttc
