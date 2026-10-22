// SPDX-License-Identifier: GPL-3.0-or-later
#include "log/Not1mmLog.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUrl>

#include "util/LogbookIndex.h"   // bandForHz

namespace ttc {

namespace {
QString envDir(const char* var, const char* fallback) {
    const QByteArray v = qgetenv(var);
    return v.isEmpty() ? QDir::homePath() + fallback : QString::fromLocal8Bit(v);
}
}  // namespace

Not1mmLog::Not1mmLog(QObject* parent, const QString& configDir, const QString& dataDir,
                     const QString& pluginDir)
    : QObject(parent),
      configDir_(configDir.isEmpty() ? envDir("XDG_CONFIG_HOME", "/.config") + "/not1mm"
                                     : configDir),
      dataDir_(dataDir.isEmpty() ? envDir("XDG_DATA_HOME", "/.local/share") + "/not1mm"
                                 : dataDir),
      pluginDir_(pluginDir) {
    timer_.setInterval(3000);
    connect(&timer_, &QTimer::timeout, this, &Not1mmLog::poll);
}

void Not1mmLog::setEnabled(bool on) {
    if (on == enabled_) return;
    enabled_ = on;
    if (on) {
        jsonMtime_ = dbMtime_ = QDateTime();       // force a read
        poll();
        timer_.start();
    } else {
        timer_.stop();
    }
    emit changed();
}

// A process whose argv names not1mm: the console script
// (~/.local/bin/not1mm, a venv's bin/not1mm) or "python -m not1mm".
bool Not1mmLog::not1mmRunning() {
    QDirIterator it("/proc", QDir::Dirs | QDir::NoDotAndDotDot);
    while (it.hasNext()) {
        it.next();
        bool num = false;
        it.fileName().toInt(&num);
        if (!num) continue;
        QFile f(it.filePath() + "/cmdline");
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QList<QByteArray> argv = f.readAll().split('\0');
        for (int i = 0; i < std::min<qsizetype>(argv.size(), 3); ++i) {
            const QByteArray a = argv[i];
            if (a == "not1mm" || a.endsWith("/not1mm")) return true;
        }
    }
    return false;
}

void Not1mmLog::poll() {
    const bool r = not1mmRunning();
    const QFileInfo js(configDir_ + "/not1mm.json");
    const QFileInfo db(dbPath_);
    const bool stale = js.lastModified() != jsonMtime_
                    || (!dbPath_.isEmpty() && db.lastModified() != dbMtime_);
    if (stale) reload();
    if (r != running_) {
        running_ = r;
        emit changed();
    }
}

QString Not1mmLog::modeGroup(const QString& m) {
    const QString u = m.toUpper();
    if (u == "LSB" || u == "USB" || u == "SSB" || u == "FM" || u == "AM") return "PH";
    if (u.startsWith("CW")) return "CW";
    if (u == "FT8" || u == "FT4" || u == "RTTY" || u.startsWith("PSK") || u.startsWith("PKT")
        || u == "JT65" || u == "JT9" || u == "Q65" || u == "MSK144" || u == "FSK441")
        return "DI";
    return "OTHER";
}

// The installed Not1MM's plugins/ folder: an explicit dir, else a
// site-packages copy, else an editable install's source tree (pip's
// direct_url.json names it — this station runs one from ~/NOT1MM).
QString Not1mmLog::findPluginDir() const {
    if (!pluginDir_.isEmpty()) return pluginDir_;
    const QString h = QDir::homePath();
    QStringList roots;
    for (const QString& base : {h + "/.venvs", h + "/.local/share/pipx/venvs",
                                h + "/.local/lib", QString("/usr/lib")}) {
        QDirIterator it(base, {"site-packages"}, QDir::Dirs, QDirIterator::Subdirectories);
        int guard = 0;
        while (it.hasNext() && ++guard < 50) roots << it.next();
    }
    for (const QString& sp : roots) {
        if (QFileInfo::exists(sp + "/not1mm/plugins")) return sp + "/not1mm/plugins";
        for (const QString& di : QDir(sp).entryList({"not1mm-*.dist-info"}, QDir::Dirs)) {
            QFile f(sp + "/" + di + "/direct_url.json");
            if (!f.open(QIODevice::ReadOnly)) continue;
            const QString url = QJsonDocument::fromJson(f.readAll()).object()
                                    .value("url").toString();
            const QString src = QUrl(url).toLocalFile();
            if (!src.isEmpty() && QFileInfo::exists(src + "/not1mm/plugins"))
                return src + "/not1mm/plugins";
        }
    }
    return QString();
}

void Not1mmLog::reload() {
    const QFileInfo js(configDir_ + "/not1mm.json");
    jsonMtime_ = js.lastModified();
    QFile jf(js.filePath());
    QJsonObject pref;
    if (jf.open(QIODevice::ReadOnly)) pref = QJsonDocument::fromJson(jf.readAll()).object();
    contestNr_ = pref.value("contest").toInt(-1);
    dbPath_ = dataDir_ + "/" + pref.value("current_database").toString("ham.db");
    dbMtime_ = QFileInfo(dbPath_).lastModified();
    calls_.clear();
    callBand_.clear();
    callBandMode_.clear();
    qsos_ = 0;
    dupeType_ = 0;
    contestName_.clear();
    if (contestNr_ >= 0 && QFileInfo::exists(dbPath_)) {
        const QString conn = QString("not1mm-%1").arg(quintptr(this));
        {
            QSqlDatabase d = QSqlDatabase::addDatabase("QSQLITE", conn);
            d.setDatabaseName(dbPath_);
            d.setConnectOptions("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=500");
            if (d.open()) {
                QSqlQuery q(d);
                q.prepare("select ContestName from ContestInstance where ContestNR = ?");
                q.addBindValue(contestNr_);
                if (q.exec() && q.next()) contestName_ = q.value(0).toString();
                q.prepare("select Call, Freq, Band, Mode from DXLOG where ContestNR = ?");
                q.addBindValue(contestNr_);
                if (q.exec())
                    while (q.next()) {
                        const QString call = q.value(0).toString().toUpper();
                        // Freq is kHz; Band (MHz, 14.0) only as a fallback.
                        qint64 hz = qint64(q.value(1).toDouble() * 1000.0);
                        if (hz <= 0) hz = qint64(q.value(2).toDouble() * 1e6) + 50000;
                        const QString band = LogbookIndex::bandForHz(hz);
                        calls_.insert(call);
                        callBand_.insert(call + '|' + band);
                        callBandMode_.insert(call + '|' + band + '|'
                                             + modeGroup(q.value(3).toString()));
                        ++qsos_;
                    }
                d.close();
            }
        }
        QSqlDatabase::removeDatabase(conn);
    }
    // The plugin's own rule ("dupe_type = 2"); unknown plugin = call+band,
    // the rule of most contests (46 of Not1MM's 95 plugins).
    if (!contestName_.isEmpty()) {
        dupeType_ = 2;
        QFile pf(findPluginDir() + "/" + contestName_ + ".py");
        if (pf.open(QIODevice::ReadOnly)) {
            static const QRegularExpression re("^dupe_type\\s*=\\s*(\\d)",
                                               QRegularExpression::MultilineOption);
            const auto m = re.match(QString::fromUtf8(pf.readAll()));
            if (m.hasMatch()) dupeType_ = m.captured(1).toInt();
        }
    }
    emit changed();
}

bool Not1mmLog::isDupe(const QString& call, qint64 hz, const QString& spotMode) const {
    if (!active()) return false;
    const QString c = call.toUpper();
    const QString band = LogbookIndex::bandForHz(hz);
    switch (dupeType_) {
    case 1: return calls_.contains(c);
    case 2: return callBand_.contains(c + '|' + band);
    case 3: {
        // The spot's mode in Not1MM's groups: SSB -> PH, FT8 -> DI.
        const QString m = spotMode == QLatin1String("SSB") ? QStringLiteral("PH")
                        : modeGroup(spotMode);
        return callBandMode_.contains(c + '|' + band + '|' + m);
    }
    default: return false;
    }
}

} // namespace ttc
