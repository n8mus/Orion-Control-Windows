// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QSet>
#include <QString>
#include <QStringList>

namespace ttc {

// DX-watch patterns, matched against every spot on the panadapter.
// Pattern grammar (whitespace or comma separated, case-insensitive):
//   W1AW      exact callsign
//   JA*       callsign prefix
//   US-1234   POTA park reference
//   NEW       any spot whose logbook status is 'N' (never worked)
//   *         everything (mostly for testing the alert path)
class WatchList {
public:
    void setPatterns(const QString& text);
    QString patterns() const { return raw_; }
    int  count() const { return n_; }
    bool empty() const { return n_ == 0; }

    // park is the POTA reference for POTA spots (empty otherwise);
    // status is the logbook worked-before code ('N'/'B'/'W'/'C'/'?').
    bool matches(const QString& call, const QString& park,
                 QChar status) const;

private:
    QString raw_;
    int n_ = 0;
    QSet<QString> exact_;                  // calls and park refs
    QStringList prefixes_;                 // from trailing-star patterns
    bool any_ = false;
    bool newOnes_ = false;
};

} // namespace ttc
