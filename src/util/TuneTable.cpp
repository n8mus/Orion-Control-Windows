// SPDX-License-Identifier: GPL-2.0-or-later
#include "util/TuneTable.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <algorithm>
#include <cmath>

namespace ttc {

namespace {
// One decimal place, the resolution of every knob that has one: keeps
// 5.2 from coming back as 5.2000000000000002 and comparing unequal.
double tenth(double v) { return std::round(v * 10.0) / 10.0; }

TunePoint clamp(TunePoint p) {
    p.hz      = TuneTable::roundKhz(p.hz);
    p.cap     = std::clamp(tenth(p.cap), 1.0, 10.0);
    p.zPos    = std::clamp(p.zPos, 1, 5);
    p.ind     = std::clamp(tenth(p.ind), 1.0, 30.0);
    p.ampTune = std::clamp(tenth(p.ampTune), 0.0, 10.0);   // Titan dials read 0-10
    p.ampLoad = std::clamp(tenth(p.ampLoad), 0.0, 10.0);
    p.swr     = p.swr >= 1.0 ? std::round(p.swr * 100.0) / 100.0 : 0.0;
    p.note    = p.note.trimmed();
    return p;
}
}  // namespace

const TunePoint* TuneTable::nearest(const QString& ant, const QString& band,
                                    qint64 hz) const {
    const auto it = map_.constFind(key(ant, band));
    if (it == map_.constEnd() || it->isEmpty()) return nullptr;
    const TunePoint* best = nullptr;
    qint64 bestD = 0;
    for (const TunePoint& p : *it) {               // sorted ascending: a tie
        const qint64 d = std::llabs(p.hz - hz);    // keeps the lower point
        if (!best || d < bestD) { best = &p; bestD = d; }
    }
    return best;
}

void TuneTable::upsert(const QString& ant, const QString& band, TunePoint pt) {
    pt = clamp(pt);
    QVector<TunePoint>& v = map_[key(ant, band)];
    const auto at = std::lower_bound(
        v.begin(), v.end(), pt.hz,
        [](const TunePoint& p, qint64 hz) { return p.hz < hz; });
    if (at != v.end() && at->hz == pt.hz) *at = pt;
    else v.insert(at, pt);
}

bool TuneTable::remove(const QString& ant, const QString& band, qint64 hz) {
    const auto it = map_.find(key(ant, band));
    if (it == map_.end()) return false;
    const qint64 k = roundKhz(hz);
    const auto n = it->removeIf([k](const TunePoint& p) { return p.hz == k; });
    if (it->isEmpty()) map_.erase(it);
    return n > 0;
}

QVector<TunePoint> TuneTable::points(const QString& ant,
                                     const QString& band) const {
    return map_.value(key(ant, band));
}

QStringList TuneTable::antennas() const {
    QStringList out;
    for (auto it = map_.constBegin(); it != map_.constEnd(); ++it) {
        const QString a = it.key().section(QChar('\x1f'), 0, 0);
        if (!out.contains(a)) out << a;
    }
    out.sort(Qt::CaseInsensitive);
    return out;
}

QStringList TuneTable::bands(const QString& ant) const {
    QStringList out;
    for (auto it = map_.constBegin(); it != map_.constEnd(); ++it)
        if (it.key().section(QChar('\x1f'), 0, 0) == ant)
            out << it.key().section(QChar('\x1f'), 1);
    return out;
}

int TuneTable::size() const {
    int n = 0;
    for (const auto& v : map_) n += static_cast<int>(v.size());
    return n;
}

// {"version":1,"points":[{"ant","band","hz","bypass","cap","z","ind",
//  "tune","load","swr","note","ts"}, ...]} — z is "L3"/"H2", one string
// for the one knob, so the file reads like the front panel.
QJsonObject TuneTable::toJson() const {
    QJsonArray arr;
    QStringList keys = map_.keys();
    keys.sort();                                   // stable file diffs
    for (const QString& k : keys) {
        const QString ant  = k.section(QChar('\x1f'), 0, 0);
        const QString band = k.section(QChar('\x1f'), 1);
        for (const TunePoint& p : map_.value(k)) {
            QJsonObject o{{"ant", ant}, {"band", band},
                          {"hz", double(p.hz)}, {"bypass", p.bypass},
                          {"cap", p.cap},
                          {"z", QString("%1%2").arg(p.highZ ? 'H' : 'L')
                                               .arg(p.zPos)},
                          {"ind", p.ind}, {"tune", p.ampTune},
                          {"load", p.ampLoad}, {"ts", double(p.savedMs)}};
            if (p.swr >= 1.0) o["swr"] = p.swr;
            if (!p.note.isEmpty()) o["note"] = p.note;
            arr.append(o);
        }
    }
    return QJsonObject{{"version", 1}, {"points", arr}};
}

void TuneTable::fromJson(const QJsonObject& root) {
    map_.clear();
    for (const QJsonValue& v : root.value("points").toArray()) {
        const QJsonObject o = v.toObject();
        TunePoint p;
        p.hz      = qint64(o.value("hz").toDouble());
        if (p.hz <= 0) continue;
        p.bypass  = o.value("bypass").toBool();
        p.cap     = o.value("cap").toDouble(1.0);
        const QString z = o.value("z").toString();
        p.highZ   = z.startsWith('H', Qt::CaseInsensitive);
        p.zPos    = z.mid(1).toInt();
        p.ind     = o.value("ind").toDouble(1.0);   // ints from before 0.1
        p.ampTune = o.value("tune").toDouble(1.0);
        p.ampLoad = o.value("load").toDouble(1.0);
        p.swr     = o.value("swr").toDouble(0.0);
        p.note    = o.value("note").toString();
        p.savedMs = qint64(o.value("ts").toDouble());
        upsert(o.value("ant").toString(), o.value("band").toString(), p);
    }
}

bool TuneTable::load(const QString& path) {
    QFile f(path);
    if (!f.exists()) { map_.clear(); return true; }
    if (!f.open(QIODevice::ReadOnly)) return false;
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return false;
    fromJson(doc.object());
    return true;
}

// Atomic (QSaveFile): a crash mid-write must never cost the operator a
// table he built up knob by knob over weeks.
bool TuneTable::save(const QString& path) const {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QJsonDocument(toJson()).toJson(QJsonDocument::Indented));
    return f.commit();
}

} // namespace ttc
