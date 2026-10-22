// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QHash>
#include <QList>
#include <QString>

class QIODevice;

namespace ttc {

// One QSO's ADIF fields, names uppercased ("CALL", "BAND", "QSO_DATE", ...).
using AdifRecord = QHash<QString, QString>;

// Minimal, tolerant ADI reader/writer. Reading: byte-oriented (ADIF lengths
// are byte counts), field values decoded as UTF-8, header discarded at <EOH>,
// records closed at <EOR>; unknown fields ride along untouched, so an import
// -> export round trip through LogDb only loses what LogDb doesn't store.
// Writing: only non-empty fields, one record per line.
namespace Adif {

QList<AdifRecord> parse(QIODevice& in);
QList<AdifRecord> parseBytes(const QByteArray& bytes);

QString writeRecord(const AdifRecord& rec);   // "<CALL:4>N8EM ... <EOR>\n"
QString fileHeader();                          // program id + <EOH>

} // namespace Adif
} // namespace ttc
