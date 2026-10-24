// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace ttc {

// Manual tuner + amp settings the operator has found by hand, per antenna
// and band, at the frequencies where he actually tuned up: the Ten-Tec 238C
// (Capacitor 1.0-10.0, one 10-position impedance knob Low Z 1-5 / High Z
// 1-5, Inductor 1.0-30.0, or BYPASS where the antenna needs no tuner) and the
// Titan III (Tune and Load, 0.0-10.0). The panadapter's tuner box shows
// the saved point nearest the TX frequency, above or below.
//
// The table grows by hand only: a save at a kHz already in the table
// replaces that entry (newest data wins), any other kHz adds one. There
// is no merge radius on purpose — the operator decides how dense each
// band needs to be by where he saves.
struct TunePoint {
    qint64  hz       = 0;       // TX frequency, stored rounded to the kHz
    bool    bypass   = false;   // tuner out of line: cap/z/ind not used
    double  cap      = 1.0;     // 1.0 .. 10.0, 0.1 steps
    bool    highZ    = false;   // impedance knob side ...
    int     zPos     = 1;       // ... and position 1-5 on that side
    double  ind      = 1.0;     // 1.0 .. 30.0, 0.1 steps
    double  ampTune  = 1.0;     // 0.0 .. 10.0, 0.1 steps
    double  ampLoad  = 1.0;     // 0.0 .. 10.0, 0.1 steps
    double  swr      = 0.0;     // wattmeter SWR at save time; 0 = none
    QString note;
    qint64  savedMs  = 0;       // epoch ms of the last save
};

class TuneTable {
public:
    static qint64 roundKhz(qint64 hz) { return (hz + 500) / 1000 * 1000; }

    // Saved point closest to hz on (antenna, band), or nullptr when that
    // antenna has nothing on that band. Equidistant: the lower one wins.
    // The pointer is valid until the table is next modified.
    const TunePoint* nearest(const QString& ant, const QString& band,
                             qint64 hz) const;
    // Insert, or replace the entry at the same kHz. Keeps the list sorted.
    void upsert(const QString& ant, const QString& band, TunePoint pt);
    bool remove(const QString& ant, const QString& band, qint64 hz);
    QVector<TunePoint> points(const QString& ant, const QString& band) const;
    // Every (antenna, band) bucket that holds at least one point.
    QStringList antennas() const;
    QStringList bands(const QString& ant) const;
    int  size() const;
    bool isEmpty() const { return size() == 0; }

    QJsonObject toJson() const;
    void fromJson(const QJsonObject& root);
    bool load(const QString& path);       // missing file = empty, true
    bool save(const QString& path) const;

private:
    static QString key(const QString& ant, const QString& band) {
        return ant + QChar('\x1f') + band;
    }
    QHash<QString, QVector<TunePoint>> map_;
};

} // namespace ttc
