// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QDateTime>
#include <QList>
#include <QObject>
#include <QString>

#include "contest/CallHistory.h"
#include "contest/ContestDef.h"

namespace ttc {

// One contest entered (an "instance"): WAE 2026 and WAE 2027 are two rows
// against the same definition, each with its own log and serial sequence.
struct ContestRow {
    qint64  id = -1;
    QString defId;               // ContestDef::id
    QString title;               // "WAE DX CW — 2026-08-08"
    QDateTime startUtc;
    QString sentExch;
    int     nextSerial = 1;
    // Cabrillo header categories.
    QString catOp = "SINGLE-OP", catAssisted = "ASSISTED", catBand = "ALL",
            catPower = "LOW", catMode = "CW", catTx = "ONE",
            catStation = "FIXED", catOverlay, club, soapbox;
};

struct ContestQso {
    qint64  id = -1;
    qint64  contestId = -1;
    QDateTime tsUtc;
    qint64  freqHz = 0;
    CQsoValues v;                // call/band/mode/exchange, engine-facing
    int     points = 0;          // display copy; export recomputes
    QString runSp = "R";
};

// The contest log — its own SQLite file, deliberately NOT the station
// logbook: no upload bookkeeping, no award state, nothing automatic. The
// contest's product is a Cabrillo file; QSOs reach the logbook only
// through an explicit post-contest push.
class ContestDb : public QObject {
    Q_OBJECT
public:
    explicit ContestDb(QObject* parent = nullptr);
    ~ContestDb() override;

    bool open(const QString& path = QString());
    bool isOpen() const;
    QString path() const { return path_; }
    static QString defaultPath();

    qint64 createContest(const ContestRow& c);      // -1 on error
    bool   updateContest(const ContestRow& c);
    QList<ContestRow> contests() const;             // newest first
    ContestRow contest(qint64 id) const;
    // Remove a contest ROW. Refuses (returns false) if it still holds
    // any QSO or QTC — a logged contest is deleted QSO-by-QSO on
    // purpose, never wholesale.
    bool deleteContest(qint64 id);

    // Serial numbering: nextSerial is what the NEXT QSO sends. Persisted
    // on every change — a crash must not replay a serial on the air.
    bool setNextSerial(qint64 contestId, int serial);

    qint64 addQso(const ContestQso& q);
    bool   updateQso(const ContestQso& q);
    bool   deleteQso(qint64 id);
    QList<ContestQso> qsos(qint64 contestId) const; // oldest first
    QList<CQsoValues> qsoValues(qint64 contestId) const;

    // Call history (one shared table — CWops membership, NAQP names…).
    // Import upserts by call inside one transaction; returns rows landed
    // or -1. Lookup is by exact call (the field force-uppercases).
    int importCallHistory(const QList<HistoryRow>& rows);
    HistoryRow historyFor(const QString& call) const;
    int historyCount() const;

    // ---- WAE QTC (sending side) ----------------------------------------
    // A confirmed block spends its QSOs forever; a QSO that has gone out
    // in a QTC can never be edited or deleted again (the stored snapshot
    // would disagree with the log) — deletes are refused by the schema
    // (FK RESTRICT), edits by updateQso itself.
    struct QtcRow {
        qint64 id = -1, qsoId = -1;
        QString toCall;
        int block = 0, item = 0;
        QDateTime tsUtc;             // when the block was confirmed
        qint64 freqHz = 0;
        QString mode;
    };
    int  qtcCount(qint64 contestId) const;          // total lines sent
    int  qtcSentTo(qint64 contestId, const QString& toCall) const;
    int  nextQtcBlock(qint64 contestId) const;      // contest-wide seq
    QSet<qint64> qtcReportedQsoIds(qint64 contestId) const;
    bool qsoReported(qint64 qsoId) const;
    // All-or-nothing: any failed insert rolls the whole block back.
    bool addQtcBlock(qint64 contestId, const QString& toCall, int block,
                     const QList<qint64>& qsoIds, qint64 freqHz,
                     const QString& mode);
    QList<QtcRow> qtcRows(qint64 contestId) const;  // confirm order

signals:
    void changed();

private:
    QString path_, conn_;
};

} // namespace ttc
