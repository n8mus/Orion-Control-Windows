// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QHash>
#include <QString>
#include <vector>

namespace ttc {

// What cty.dat knows about a callsign's country.
struct CtyInfo {
    QString country;
    QString cont;                   // "EU", "NA", "AS", "AF", "OC", "SA", "AN"
    int cq = 0, itu = 0;
    double lat = 0.0, lon = 0.0;    // east-positive
};

// Callsign -> approximate location via the AD1C cty.dat country file
// (bundled as a Qt resource). Exact-call entries ("=N8EM") win, then the
// longest matching prefix. Coordinates come back east-positive (cty.dat
// stores longitude west-positive; the sign is flipped on load).
class CtyLookup {
public:
    bool load(const QString& path = ":/cty.dat");
    bool loaded() const { return !prefixes_.empty(); }
    bool lookup(const QString& call, double& lat, double& lon) const;
    bool info(const QString& call, CtyInfo& out) const;

    // Maidenhead grid (4 or 6 chars) -> center of the square, east-positive.
    static bool gridToLatLon(const QString& grid, double& lat, double& lon);

    // CQ zone of a US state (2-letter USPS code), 0 if unknown. In the
    // lower 48 the CQ zone is a clean function of the state, so this is
    // the accurate fill when a QRZ profile omits its zone — which most
    // US ops do. AK=1, HI=31; everything else is 3/4/5.
    static int usStateCqZone(const QString& state);

private:
    int find(const QString& call) const;           // country index, -1 none
    struct Country { QString name, cont; int cq, itu; float lat, lon; };
    // An alias carries the country plus optional per-prefix zone
    // overrides — cty.dat's "(cq)[itu]" decorations, e.g. W6 in the US
    // is CQ zone 3 while the country default is 5. -1 = no override.
    struct Ent { QString pfx; quint16 ci; qint16 cq = -1, itu = -1; };
    std::vector<Country> countries_;
    std::vector<Ent> prefixes_;                    // all aliases, all countries
    QHash<QString, Ent> exact_;                    // "=CALL" overrides
};

} // namespace ttc
