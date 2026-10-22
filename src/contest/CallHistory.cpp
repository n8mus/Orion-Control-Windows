// SPDX-License-Identifier: GPL-2.0-or-later
#include "contest/CallHistory.h"

#include <QHash>
#include <QIODevice>

#include <algorithm>

namespace ttc {

QList<HistoryRow> parseCallHistory(QIODevice& in, QString* err) {
    QList<HistoryRow> out;
    QHash<QString, int> col;         // canonical header name -> position
    bool haveHeader = false;
    int lineNo = 0;
    while (!in.atEnd()) {
        ++lineNo;
        const QString line =
            QString::fromUtf8(in.readLine()).trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        if (!haveHeader) {
            // Header: "!!Order!!,Call,Name,Exch1,UserText" — the
            // !!Order!! marker is optional decoration on the first cell.
            QString h = line;
            h.remove("!!Order!!");
            if (h.startsWith(',')) h.remove(0, 1);
            const QStringList names = h.split(',');
            for (int i = 0; i < names.size(); ++i) {
                const QString n = names[i].trimmed().toLower();
                if (n.isEmpty()) continue;
                if (col.contains(n)) {
                    if (err)
                        *err = QString(
                            "duplicate column \"%1\" in the header — a "
                            "name-keyed import would silently drop one of "
                            "them (the CWops two-Misc bug). Fix the header "
                            "before importing.").arg(names[i].trimmed());
                    return {};
                }
                col.insert(n, i);
            }
            if (!col.contains("call")) {
                if (err)
                    *err = QString("no Call column in the header "
                                   "(line %1)").arg(lineNo);
                return {};
            }
            haveHeader = true;
            continue;
        }
        const QStringList f = line.split(',');
        const auto get = [&](const char* name) {
            const int i = col.value(QLatin1String(name), -1);
            return (i >= 0 && i < f.size()) ? f[i].trimmed() : QString();
        };
        HistoryRow r;
        r.call = get("call").toUpper();
        if (r.call.isEmpty()) continue;
        r.name = get("name");
        r.exch1 = get("exch1");
        r.sect = get("sect");
        r.state = get("state");
        r.grid = get("loc1");
        r.ck = get("ck");
        r.power = get("power");
        r.userText = get("usertext");
        out << r;
    }
    if (!haveHeader && err) *err = "no header line found";
    return out;
}

QStringList scpMatches(const QString& partial, const QSet<QString>& scp,
                       int max) {
    const QString p = partial.trimmed().toUpper();
    if (p.size() < 2) return {};
    QStringList pre, sub;
    for (const QString& c : scp) {
        if (c.startsWith(p)) pre << c;
        else if (c.contains(p)) sub << c;
    }
    std::sort(pre.begin(), pre.end());
    std::sort(sub.begin(), sub.end());
    QStringList out = pre;
    out += sub;
    if (out.size() > max) out = out.mid(0, max);
    return out;
}

} // namespace ttc
