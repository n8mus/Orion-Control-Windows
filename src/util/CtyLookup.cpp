// SPDX-License-Identifier: GPL-2.0-or-later
#include "util/CtyLookup.h"

#include <QFile>

namespace ttc {

// cty.dat: a country header line
//   "United States:  05:  08:  NA:   39.00:    98.00:     5.0:  K:"
// followed by continuation lines of comma-separated aliases ending in ';'.
// Aliases may carry decorations — (cq)[itu]<lat/lon>{cont}~tz~ — which are
// stripped; "=CALL" marks an exact-callsign entry.
bool CtyLookup::load(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
    countries_.clear();
    prefixes_.clear();
    exact_.clear();
    bool haveCountry = false;
    while (!f.atEnd()) {
        const QString line = QString::fromLatin1(f.readLine());
        if (line.isEmpty()) continue;
        if (!line.startsWith(' ') && line.contains(':')) {   // country header
            const QStringList fields = line.split(':');
            if (fields.size() >= 7) {
                Country c;
                c.name = fields[0].trimmed();
                c.cq   = fields[1].trimmed().toInt();
                c.itu  = fields[2].trimmed().toInt();
                c.lat  = fields[4].trimmed().toFloat();
                c.lon  = -fields[5].trimmed().toFloat();     // west-positive -> east
                countries_.push_back(c);
                haveCountry = true;
            }
            continue;
        }
        if (!haveCountry) continue;
        const auto ci = quint16(countries_.size() - 1);
        for (QString tok : line.trimmed().remove(';').split(',', Qt::SkipEmptyParts)) {
            // Strip per-alias override decorations.
            for (const QChar cut : {QChar('('), QChar('['), QChar('<'),
                                    QChar('{'), QChar('~')}) {
                const int i = tok.indexOf(cut);
                if (i >= 0) tok.truncate(i);
            }
            tok = tok.trimmed().toUpper();
            if (tok.isEmpty()) continue;
            if (tok.startsWith('=')) exact_.insert(tok.mid(1), ci);
            else                     prefixes_.push_back({tok, ci});
        }
    }
    return !prefixes_.empty();
}

int CtyLookup::find(const QString& call) const {
    const QString c = call.trimmed().toUpper();
    if (c.isEmpty()) return -1;
    if (const auto it = exact_.constFind(c); it != exact_.constEnd())
        return *it;
    int bestLen = 0, best = -1;
    for (const Ent& e : prefixes_)
        if (e.pfx.size() > bestLen && c.startsWith(e.pfx)) {
            best = e.ci;
            bestLen = e.pfx.size();
        }
    return best;
}

bool CtyLookup::lookup(const QString& call, double& lat, double& lon) const {
    const int ci = find(call);
    if (ci < 0) return false;
    lat = countries_[size_t(ci)].lat;
    lon = countries_[size_t(ci)].lon;
    return true;
}

bool CtyLookup::info(const QString& call, CtyInfo& out) const {
    const int ci = find(call);
    if (ci < 0) return false;
    const Country& c = countries_[size_t(ci)];
    out.country = c.name;
    out.cq  = c.cq;
    out.itu = c.itu;
    out.lat = c.lat;
    out.lon = c.lon;
    return true;
}

bool CtyLookup::gridToLatLon(const QString& grid, double& lat, double& lon) {
    const QString g = grid.trimmed().toUpper();
    if (g.size() < 4 || !g[0].isLetter() || !g[1].isLetter()
        || !g[2].isDigit() || !g[3].isDigit())
        return false;
    lon = (g[0].toLatin1() - 'A') * 20.0 - 180.0 + (g[2].toLatin1() - '0') * 2.0;
    lat = (g[1].toLatin1() - 'A') * 10.0 - 90.0  + (g[3].toLatin1() - '0') * 1.0;
    if (g.size() >= 6 && g[4].isLetter() && g[5].isLetter()) {
        lon += (g[4].toLatin1() - 'A') * 2.0 / 24.0 + 1.0 / 24.0;
        lat += (g[5].toLatin1() - 'A') * 1.0 / 24.0 + 0.5 / 24.0;
    } else {
        lon += 1.0;                                // center of the 4-char square
        lat += 0.5;
    }
    return true;
}

} // namespace ttc
