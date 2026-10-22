// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QDateTime>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>

namespace ttc {

// The running Not1MM contest's dupe sheet, for graying worked stations on
// the band map. Not1MM (github.com/mbridak/not1mm) is the master of its
// log, so this only READS it: its settings file names the current contest
// (not1mm.json "contest" = ContestNR, "current_database"), its SQLite log
// (~/.local/share/not1mm/<db>, table DXLOG) holds that contest's QSOs, and
// the contest's plugin carries the rule Not1MM itself applies
// ("dupe_type": 1 call, 2 call+band, 3 call+band+mode group, 4/5 none we
// can judge). Re-read whenever either file changes (polled mtimes — the
// log is rewritten in place, the JSON replaced), and only consulted while
// a Not1MM process is running, so last weekend's contest doesn't gray
// tonight's band map. The console's own contest deck, when open, wins.
class Not1mmLog : public QObject {
    Q_OBJECT
public:
    // Empty dirs = the defaults (XDG config / data, the installed plugins).
    explicit Not1mmLog(QObject* parent = nullptr, const QString& configDir = {},
                       const QString& dataDir = {}, const QString& pluginDir = {});

    void setEnabled(bool on);
    bool enabled() const { return enabled_; }
    // Not1MM running AND a contest loaded with a dupe rule we can apply.
    bool active() const { return enabled_ && running_ && dupeType_ >= 1 && dupeType_ <= 3; }
    // spotMode: guessSpotMode()'s answer ("CW", "SSB", "FT8"...).
    bool isDupe(const QString& call, qint64 hz, const QString& spotMode) const;

    QString contestName() const { return contestName_; }
    int dupeType() const { return dupeType_; }
    int qsoCount() const { return qsos_; }

    void reload();                                 // public for not1mmtest
    void setRunningForTest(bool r) { running_ = r; }

signals:
    void changed();                                // dupe sheet or active() moved

private:
    void poll();
    static bool not1mmRunning();
    static QString modeGroup(const QString& m);    // Not1MM's PH/CW/DI/OTHER
    QString findPluginDir() const;

    QString configDir_, dataDir_, pluginDir_;
    bool enabled_ = false, running_ = false;
    QTimer timer_;
    QDateTime jsonMtime_, dbMtime_;
    QString dbPath_;
    int contestNr_ = -1, dupeType_ = 0, qsos_ = 0;
    QString contestName_;
    QSet<QString> calls_, callBand_, callBandMode_;
};

} // namespace ttc
