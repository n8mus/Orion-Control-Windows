// SPDX-License-Identifier: GPL-2.0-or-later
#include "util/LogbookIndex.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>

#include "log/LogDb.h"
#include "util/CtyLookup.h"

namespace ttc {

namespace {
// TTC_CQRLOG_HOME overrides for tests (points at a sandboxed copy).
QString cqrlogHome() {
    const QByteArray env = qgetenv("TTC_CQRLOG_HOME");
    return env.isEmpty() ? QDir::homePath() : QString::fromUtf8(env);
}
// cqrlog names its databases cqrlog001, cqrlog002… after the log number in
// cqrlog_login.cfg. LastOpenedLog is what the auto-open uses; follow it.
QString cqrlogDbName() {
    QSettings login(cqrlogHome() + "/.config/cqrlog/cqrlog_login.cfg",
                    QSettings::IniFormat);
    const int n = login.value("cqrlog/LastOpenedLog",
                              login.value("LastOpenedLog", 1)).toInt();
    return QString("cqrlog%1").arg(n, 3, 10, QChar('0'));
}
QString cqrlogSocket() {
    return cqrlogHome() + "/.config/cqrlog/database/sock";
}
} // namespace

LogbookIndex::LogbookIndex(QObject* parent) : QObject(parent) {
    timer_ = new QTimer(this);
    timer_->setInterval(5 * 60 * 1000);    // retry/refresh while console runs
    connect(timer_, &QTimer::timeout, this, [this] { refresh(); });
}

void LogbookIndex::start() {
    QFile cache(cachePath());
    if (cache.open(QIODevice::ReadOnly | QIODevice::Text)) {
        parseRows(QString::fromUtf8(cache.readAll()));
        if (haveData_) emit updated();
    }
    timer_->start();
    refresh();
}

void LogbookIndex::refreshSoon(int delayMs) {
    QTimer::singleShot(delayMs, this, [this] { refresh(); });
}

void LogbookIndex::refresh() {
    if (proc_) return;                     // one at a time
    if (!QFile::exists(cqrlogSocket())) return;   // cqrlog not running
    proc_ = new QProcess(this);
    const QString sql =
        "SELECT 'W', callsign, band FROM cqrlog_main "
        "UNION SELECT 'C', callsign, band FROM cqrlog_main "
        " WHERE qsl_r='Q' OR lotw_qslr='L' OR eqsl_qsl_rcvd='E' "
        "UNION SELECT 'P', pota_hunted_ref, '' FROM cqrlog_main "
        " WHERE pota_hunted_ref IS NOT NULL AND pota_hunted_ref<>'';";
    connect(proc_, &QProcess::finished, this,
            [this](int code, QProcess::ExitStatus st) {
        const QString out = QString::fromUtf8(proc_->readAllStandardOutput());
        proc_->deleteLater();
        proc_ = nullptr;
        if (st != QProcess::NormalExit || code != 0 || out.isEmpty()) return;
        parseRows(out);
        QFile cache(cachePath());
        if (cache.open(QIODevice::WriteOnly | QIODevice::Text))
            cache.write(out.toUtf8());
        emit updated();
    });
    proc_->start("mysql",
                 {"--socket=" + cqrlogSocket(), "-u", "root", cqrlogDbName(),
                  "-N", "-B", "--connect-timeout=3", "-e", sql});
    QTimer::singleShot(15000, proc_, [p = proc_] {
        if (p->state() != QProcess::NotRunning) p->kill();
    });
}

void LogbookIndex::parseRows(const QString& tsv) {
    QSet<QString> wc, wcb, ccb, pk;
    for (const QString& line : tsv.split('\n', Qt::SkipEmptyParts)) {
        const QStringList f = line.split('\t');
        if (f.size() < 2) continue;
        const QString call = f[1].trimmed().toUpper();
        const QString band = f.size() > 2 ? f[2].trimmed().toUpper() : QString();
        if (call.isEmpty() || call == "NULL") continue;
        if (f[0] == "W") {
            wc.insert(call);
            wcb.insert(call + '|' + band);
        } else if (f[0] == "C") {
            ccb.insert(call + '|' + band);
        } else if (f[0] == "P") {
            // pota_hunted_ref can hold a comma list (N-fer park-to-park)
            for (const QString& p : call.split(',', Qt::SkipEmptyParts))
                pk.insert(p.trimmed());
        }
    }
    if (wc.isEmpty() && pk.isEmpty()) return;      // don't wipe on bad read
    cqrWorkedCall_ = std::move(wc);
    cqrWorkedCallBand_ = std::move(wcb);
    cqrConfCallBand_ = std::move(ccb);
    cqrParks_ = std::move(pk);
    rebuildMerged();
}

void LogbookIndex::attachDb(LogDb* db) {
    db_ = db;
    if (!db_) return;
    connect(db_, &LogDb::changed, this, [this] { refreshDb(); });
    refreshDb();
}

void LogbookIndex::attachCty(const CtyLookup* cty) {
    cty_ = cty;
    ctyMemo_.clear();
}

QString LogbookIndex::countryOf(const QString& call) const {
    const QString c = call.trimmed().toUpper();
    if (c.isEmpty()) return QString();
    // A logged call's stored country beats prefix guessing.
    if (const auto it = dbCallCountry_.constFind(c);
        it != dbCallCountry_.constEnd())
        return *it;
    if (const auto it = ctyMemo_.constFind(c); it != ctyMemo_.constEnd())
        return *it;
    QString name;
    if (cty_) {
        CtyInfo ci;
        if (cty_->info(c, ci)) name = ci.country;
    }
    ctyMemo_.insert(c, name);
    return name;
}

void LogbookIndex::refreshDb() {
    if (!db_) return;
    dbCallCountry_.clear();
    ctyMemo_.clear();
    dbWorkedCall_.clear();
    dbWorkedCallBand_.clear();
    dbConfCallBand_.clear();
    dbParks_.clear();
    workedCountry_.clear();
    confCountry_.clear();
    workedCountryBand_.clear();
    confCountryBand_.clear();
    workedCountryMode_.clear();
    confCountryMode_.clear();
    haveDbData_ = false;
    const auto rows = db_->workedRows();
    for (const LogDb::WorkedRow& r : rows) {
        const QString call = r.call.trimmed().toUpper();
        if (call.isEmpty()) continue;
        haveDbData_ = true;
        const QString band = r.band.trimmed().toUpper();
        dbWorkedCall_.insert(call);
        dbWorkedCallBand_.insert(call + '|' + band);
        if (r.conf) dbConfCallBand_.insert(call + '|' + band);
        if (!r.park.isEmpty())
            for (const QString& p : r.park.split(',', Qt::SkipEmptyParts))
                dbParks_.insert(p.trimmed().toUpper());
        // Country dimensions: stored country wins; resolve via cty.dat for
        // rows imported before the country column was stamped.
        QString ctry = r.country.trimmed();
        if (!ctry.isEmpty()) dbCallCountry_.insert(call, ctry);
        else ctry = countryOf(call);
        if (ctry.isEmpty()) continue;
        const QString mode = r.mode.trimmed().toUpper();
        workedCountry_.insert(ctry);
        workedCountryBand_.insert(ctry + '|' + band);
        workedCountryMode_.insert(ctry + '|' + mode);
        if (r.conf) {
            confCountry_.insert(ctry);
            confCountryBand_.insert(ctry + '|' + band);
            confCountryMode_.insert(ctry + '|' + mode);
        }
    }
    rebuildMerged();
    emit updated();
}

void LogbookIndex::rebuildMerged() {
    workedCall_ = cqrWorkedCall_ + dbWorkedCall_;
    workedCallBand_ = cqrWorkedCallBand_ + dbWorkedCallBand_;
    confCallBand_ = cqrConfCallBand_ + dbConfCallBand_;
    parks_ = cqrParks_ + dbParks_;
    haveData_ = !workedCall_.isEmpty() || !parks_.isEmpty();
}

QChar LogbookIndex::status(const QString& call, const QString& band) const {
    if (!haveData_) return QChar('?');
    const QString c = call.trimmed().toUpper();
    if (!workedCall_.contains(c)) return QChar('N');
    const QString key = c + '|' + band;
    if (confCallBand_.contains(key)) return QChar('C');
    if (workedCallBand_.contains(key)) return QChar('W');
    return QChar('B');
}

LogbookIndex::Need LogbookIndex::need(const QString& call, const QString& band,
                                      const QString& mode) const {
    Need out;
    if (!haveDbData_) return out;
    const QString ctry = countryOf(call);
    if (ctry.isEmpty()) return out;
    const auto dim = [](const QSet<QString>& conf, const QSet<QString>& worked,
                        const QString& key) {
        if (conf.contains(key)) return QChar('C');
        if (worked.contains(key)) return QChar('W');
        return QChar('N');
    };
    out.country = dim(confCountry_, workedCountry_, ctry);
    const QString b = band.trimmed().toUpper();
    if (!b.isEmpty())
        out.band = dim(confCountryBand_, workedCountryBand_, ctry + '|' + b);
    const QString m = mode.trimmed().toUpper();
    if (!m.isEmpty())
        out.mode = dim(confCountryMode_, workedCountryMode_, ctry + '|' + m);
    return out;
}

bool LogbookIndex::parkHunted(const QString& park) const {
    return haveData_ && parks_.contains(park.trimmed().toUpper());
}

QString LogbookIndex::bandForHz(qint64 hz) {
    struct B { qint64 lo, hi; const char* name; };
    static const B bands[] = {
        {1800000, 2000000, "160M"}, {3500000, 4000000, "80M"},
        {5250000, 5450000, "60M"},  {7000000, 7300000, "40M"},
        {10100000, 10150000, "30M"}, {14000000, 14350000, "20M"},
        {18068000, 18168000, "17M"}, {21000000, 21450000, "15M"},
        {24890000, 24990000, "12M"}, {28000000, 29700000, "10M"},
        {50000000, 54000000, "6M"},  {144000000, 148000000, "2M"},
        {420000000, 450000000, "70CM"}, {902000000, 928000000, "33CM"},
    };
    for (const B& b : bands)
        if (hz >= b.lo && hz <= b.hi) return b.name;
    return QString();
}

QString LogbookIndex::cachePath() const {
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir + "/logbook-index.tsv";
}

} // namespace ttc
