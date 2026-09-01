// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QHash>
#include <QString>
#include <vector>

namespace ttc {

// What cty.dat knows about a callsign's country.
struct CtyInfo {
    QString country;
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

private:
    int find(const QString& call) const;           // country index, -1 none
    struct Country { QString name; int cq, itu; float lat, lon; };
    struct Ent { QString pfx; quint16 ci; };
    std::vector<Country> countries_;
    std::vector<Ent> prefixes_;                    // all aliases, all countries
    QHash<QString, quint16> exact_;                // "=CALL" overrides
};

} // namespace ttc
