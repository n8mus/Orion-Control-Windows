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
                c.cont = fields[3].trimmed();
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
            // Pull the per-alias zone overrides BEFORE stripping them:
            // "(cq)" and "[itu]" refine the country default for this
            // prefix (the CQ-zone boundaries inside big countries).
            qint16 cqOv = -1, ituOv = -1;
            if (const int a = tok.indexOf('('); a >= 0) {
                const int b = tok.indexOf(')', a);
                if (b > a) cqOv = qint16(tok.mid(a + 1, b - a - 1).toInt());
            }
            if (const int a = tok.indexOf('['); a >= 0) {
                const int b = tok.indexOf(']', a);
                if (b > a) ituOv = qint16(tok.mid(a + 1, b - a - 1).toInt());
            }
            for (const QChar cut : {QChar('('), QChar('['), QChar('<'),
                                    QChar('{'), QChar('~')}) {
                const int i = tok.indexOf(cut);
                if (i >= 0) tok.truncate(i);
            }
            tok = tok.trimmed().toUpper();
            if (tok.isEmpty()) continue;
            if (tok.startsWith('='))
                exact_.insert(tok.mid(1), {tok.mid(1), ci, cqOv, ituOv});
            else
                prefixes_.push_back({tok, ci, cqOv, ituOv});
        }
    }
    return !prefixes_.empty();
}

int CtyLookup::find(const QString& call) const {
    const QString c = call.trimmed().toUpper();
    if (c.isEmpty()) return -1;
    if (const auto it = exact_.constFind(c); it != exact_.constEnd())
        return it->ci;
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
    const QString cc = call.trimmed().toUpper();
    qint16 cqOv = -1, ituOv = -1;
    int ci = -1;
    if (const auto it = exact_.constFind(cc); it != exact_.constEnd()) {
        ci = it->ci;
        cqOv = it->cq;
        ituOv = it->itu;
    } else {
        int bestLen = 0;
        for (const Ent& e : prefixes_)
            if (e.pfx.size() > bestLen && cc.startsWith(e.pfx)) {
                ci = e.ci;
                cqOv = e.cq;
                ituOv = e.itu;
                bestLen = e.pfx.size();
            }
    }
    if (ci < 0) return false;
    const Country& c = countries_[size_t(ci)];
    out.country = c.name;
    out.cont = c.cont;
    out.cq  = cqOv >= 0 ? cqOv : c.cq;    // per-prefix zone wins
    out.itu = ituOv >= 0 ? ituOv : c.itu;
    out.lat = c.lat;
    out.lon = c.lon;
    return true;
}

int CtyLookup::usStateCqZone(const QString& state) {
    // CQ WW US zones: 3 is the far-western tier, 5 is the eastern seaboard
    // (New England down through the SE coast + the Appalachian belt), and 4
    // is EVERYTHING between — the whole Midwest (MI, OH, IN, IL, WI, KY,
    // TN, AL, MS) is zone 4, NOT 5. The 4/5 line is the Appalachians, not
    // the Mississippi. (Michigan is 4 — the operator's own state.)
    static const QHash<QString, int> z = {
        // Zone 3 — Pacific + interior west.
        {"AZ",3},{"CA",3},{"ID",3},{"NV",3},{"OR",3},{"UT",3},{"WA",3},
        // Zone 4 — Great Plains, Midwest, and the western South.
        {"AL",4},{"AR",4},{"CO",4},{"IA",4},{"IL",4},{"IN",4},{"KS",4},
        {"KY",4},{"LA",4},{"MI",4},{"MN",4},{"MO",4},{"MS",4},{"MT",4},
        {"ND",4},{"NE",4},{"NM",4},{"OH",4},{"OK",4},{"SD",4},{"TN",4},
        {"TX",4},{"WI",4},{"WY",4},
        // Zone 5 — New England, Mid-Atlantic, and the Southeast coast.
        {"CT",5},{"DC",5},{"DE",5},{"FL",5},{"GA",5},{"MA",5},{"MD",5},
        {"ME",5},{"NC",5},{"NH",5},{"NJ",5},{"NY",5},{"PA",5},{"RI",5},
        {"SC",5},{"VA",5},{"VT",5},{"WV",5},
        {"AK",1},{"HI",31},
    };
    return z.value(state.trimmed().toUpper(), 0);
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
