// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

class QIODevice;

namespace ttc {

// One call-history entry (the N1MM/Not1MM call-history file model).
struct HistoryRow {
    QString call, name, exch1, sect, state, grid, ck, power, userText;
};

// Parse a call-history file: a header line naming the columns
// ("!!Order!!,Call,Name,Exch1,..."), then comma-separated rows. Comment
// lines (#) are skipped.
//
// Columns are mapped by an EXPLICIT header inspection and the parse
// REFUSES a file whose header repeats a column name. That rule is paid
// for: the published CWops list labels two different columns "Misc", a
// name-keyed dict silently dropped the member number, and the CWT
// exchange prefilled the STATE for a month before anyone noticed.
QList<HistoryRow> parseCallHistory(QIODevice& in, QString* err);

// Check-partial ordering: calls beginning with the partial first, then
// calls containing it, alphabetical within each group, capped at max.
// Pure ranking — the caller decorates worked/history status itself.
QStringList scpMatches(const QString& partial, const QSet<QString>& scp,
                       int max = 30);

} // namespace ttc
