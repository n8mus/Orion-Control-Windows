// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QDialog>
#include <QList>

#include "contest/ContestDb.h"
#include "log/LogDb.h"           // Qso — the everyday log's row

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QWidget;

namespace ttc {

class CtyLookup;
class LogDb;

// The contest MANAGER — the between-runs screen. Per-QSO operating
// lives on the ContestDeck; this window starts/resumes contests, shows
// the full log with edit/delete (both refused for QSOs already reported
// in a QTC — the receiving station holds a copy of what we logged), and
// writes the verified Cabrillo.
class ContestWindow : public QDialog {
    Q_OBJECT
public:
    // logDb may be null (tests): the push-to-logbook button then warns.
    ContestWindow(ContestDb* db, const CtyLookup* cty,
                  LogDb* logDb = nullptr, QWidget* parent = nullptr);

    bool openContestId(qint64 id);

signals:
    // The deck follows whichever contest this manager opens or creates.
    void contestOpened(qint64 id);
    // Fresh QSOs just landed in the everyday log (station-log ids,
    // dup-skips absent) — the owner wakes the online-log sweep and
    // mirrors each one onward to cqrlog's bridge.
    void pushedToLogbook(const QList<qint64>& ids);
    // A contest was removed — if the deck is showing it, close it.
    void contestDeleted(qint64 id);

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    void buildUi();
    void refreshResumeList();
    void newContest();
    void openContest(qint64 id);
    void deleteContestRow();         // the Delete button on the resume row
    void refreshAll();
    void editSelected();
    void deleteSelected();
    void exportCabrillo();
    // Contest QSOs -> the everyday station log. The contest db stays
    // authoritative for the contest; the logbook copy feeds awards,
    // LoTW and the worked-before colors. Near-dupe guarded: pushing
    // twice cannot double a QSO.
    void pushToLogbook();
    void exportAdif();                       // plain .adi file
    Qso logbookQso(const ContestQso& q) const;
    qint64 selectedQsoId() const;
    void trace(const QString& line);

    ContestDb* db_;
    const CtyLookup* cty_;
    LogDb* logDb_ = nullptr;
    qint64 contestId_ = -1;
    const ContestDef* def_ = nullptr;
    ContestRow row_;
    ContestContext ctx_;
    QList<ContestQso> qsos_;

    QComboBox* defPick_ = nullptr;
    QComboBox* resumePick_ = nullptr;
    QLineEdit* sentExchEdit_ = nullptr;   // New-contest row
    QLineEdit* openExchEdit_ = nullptr;   // open contest, editable
    QLineEdit* locationEdit_ = nullptr;
    QLineEdit* emailEdit_ = nullptr;      // Cabrillo EMAIL:
    QLabel* title_ = nullptr;
    QLabel* score_ = nullptr;
    QTableWidget* table_ = nullptr;
    QLabel* status_ = nullptr;
};

} // namespace ttc
