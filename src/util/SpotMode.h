// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QString>

namespace ttc {

// Best-effort mode for a spot, for the country/band/mode needed-status:
// the kind knows (skimmer = CW, FT8 feed = FT8), the comment often says,
// and the band plan's phone edge settles the rest.
inline QString guessSpotMode(char kind, const QString& comment, qint64 hz) {
    if (kind == 'F') return QStringLiteral("FT8");
    if (kind == 'S') return QStringLiteral("CW");
    const QString c = comment.toUpper();
    if (c.contains(QLatin1String("FT8")) || c.contains(QLatin1String("FT4")))
        return QStringLiteral("FT8");
    if (c.contains(QLatin1String("RTTY"))) return QStringLiteral("RTTY");
    if (c.contains(QLatin1String("CW"))) return QStringLiteral("CW");
    if (c.contains(QLatin1String("SSB")) || c.contains(QLatin1String("USB"))
        || c.contains(QLatin1String("LSB")))
        return QStringLiteral("SSB");
    struct Edge { qint64 lo, phone; };
    static const Edge edges[] = {
        {1800000, 1843000},   {3500000, 3600000},   {7000000, 7125000},
        {10100000, 10150000}, {14000000, 14150000}, {18068000, 18110000},
        {21000000, 21200000}, {24890000, 24930000}, {28000000, 28300000},
        {50000000, 50100000},
    };
    for (const Edge& e : edges)
        if (hz >= e.lo && hz < e.phone) return QStringLiteral("CW");
    return QStringLiteral("SSB");
}

} // namespace ttc
