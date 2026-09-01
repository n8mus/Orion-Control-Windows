// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QDateTime>
#include <QObject>
#include <QSqlDatabase>
#include <QString>

#include "log/Adif.h"

class QIODevice;

namespace ttc {

class CtyLookup;

// One logged contact. Times are UTC. band is cqrlog-style text ("20M"),
// mode is the ADIF mode ("CW", "SSB", "FT8"...). The qsl/lotw/eqsl flags
// hold "" or "Y" (received); upload bookkeeping columns exist in the schema
// for the online-log push (v2) but aren't surfaced here yet.
struct Qso {
    qint64    id = -1;
    QDateTime tsUtc;
    QString   call, band, mode;
    qint64    freqHz = 0;
    QString   rstS, rstR, name, qth, grid, country;
    int       cqz = 0, ituz = 0;
    QString   pota, comment;
    QString   qslRcvd, lotwRcvd, eqslRcvd;
};

// The station log: one SQLite file (default under the app data dir,
// override with log/dbPath). This is the console's own logbook — cqrlog on
// the Linux box stays the award engine, fed by ADIF export from here.
class LogDb : public QObject {
    Q_OBJECT
public:
    explicit LogDb(QObject* parent = nullptr);
    ~LogDb() override;

    bool open(const QString& path = QString());
    bool isOpen() const;
    QString path() const { return path_; }
    static QString defaultPath();
    QSqlDatabase database() const;         // for table models on this log
    void notifyExternalChange() { emit changed(); }   // model-side edits

    qint64 addQso(const Qso& q);           // returns new id, -1 on error
    bool   updateQso(const Qso& q);
    bool   deleteQso(qint64 id);
    Qso    qso(qint64 id) const;
    QList<Qso> prevQsos(const QString& call, int limit = 8) const;
    int    count() const;

    // Same call+band+mode within 5 minutes already in the log.
    bool hasNearDuplicate(const Qso& q) const;

    // ADIF. Import skips near-duplicates (same call+band+mode within
    // 5 minutes) and stamps country/zones via cty when the record has none.
    int  importAdif(QIODevice& in, const CtyLookup* cty, QString* err = nullptr);
    bool exportAdif(QIODevice& out) const;
    static AdifRecord toAdif(const Qso& q);
    static Qso fromAdif(const AdifRecord& rec);

    // Bulk feed for LogbookIndex (worked/confirmed sets).
    struct WorkedRow {
        QString call, band, mode, country, park;
        bool conf = false;
    };
    QList<WorkedRow> workedRows() const;

    // Online-log push bookkeeping. svc is one of "lotw", "eqsl", "qrz",
    // "club", "hrdlog" (the up_* columns). State: ' ' pending, 'Y' sent,
    // 'E' last attempt failed (retried later).
    bool setUploadState(qint64 id, const QString& svc, QChar st);
    QList<Qso> pendingUploads(const QString& svc, int limit = 50) const;

signals:
    void changed();                        // any insert/update/delete/import

private:
    QString path_;
    QString conn_;                         // Qt SQL connection name
};

} // namespace ttc
