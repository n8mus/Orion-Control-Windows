// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QList>
#include <QString>

#include "contest/ContestDb.h"

namespace ttc {

class CtyLookup;

// Station identity for the Cabrillo header — read from the console's
// Station setup, passed in so this stays a pure formatter.
struct CabrilloStation {
    QString call, name, address, gridLocator, location, email, club;
};

// The submission file. Contest software fails at submission time, weeks
// after the mistake, so this module is paranoid by construction:
//  - QSO lines come from the def's ONE cabExch token list, applied to
//    both sides — the sent/received field orders cannot drift.
//  - build() recomputes claimed score; the display never leaks in.
//  - selfCheck() parses the finished text back and compares every QSO
//    against the database rows. Run before every write; a mismatch means
//    the file lies about the log and must not be submitted.
// CRLF line endings per spec.
class Cabrillo {
public:
    // qtcs may be empty (every contest but WAE). QTC: lines interleave
    // with QSO: lines chronologically, receiver's call first, per the
    // WWROF WAE template — the field order the DARC robot parses.
    static QString build(const ContestDef& def, const ContestRow& contest,
                         const QList<ContestQso>& qsos,
                         const QList<ContestDb::QtcRow>& qtcs,
                         const CabrilloStation& st, const CtyLookup* cty,
                         const ContestContext& ctx);

    // Parse-back verification of build()'s output. Returns true when
    // every QSO: and QTC: line round-trips to its database row; on
    // failure err names the first offending line.
    static bool selfCheck(const QString& text, const ContestDef& def,
                          const ContestRow& contest,
                          const QList<ContestQso>& qsos,
                          const QList<ContestDb::QtcRow>& qtcs,
                          QString* err);

    // "  7024" — kHz right-aligned in 5, the classic QSO: line freq.
    static QString freqField(qint64 hz);
    // One side's exchange tokens for a QSO ("599 001"), per cabExch.
    static QStringList exchTokens(const ContestDef& def,
                                  const ContestRow& contest,
                                  const ContestQso& q, bool sentSide);
};

} // namespace ttc
