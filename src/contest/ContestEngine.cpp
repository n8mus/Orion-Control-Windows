// SPDX-License-Identifier: GPL-2.0-or-later
#include "contest/ContestEngine.h"

#include <algorithm>
#include <cmath>

#include <QLocale>
#include <QTimeZone>

#include "util/CtyLookup.h"

namespace ttc {

QString normalizeForCty(const QString& call) {
    QString c = call.trimmed().toUpper();
    if (!c.contains('/')) return c;
    // Portable tails that say HOW someone operates, not WHERE from.
    static const QSet<QString> kTails = {
        "M", "MM", "P", "QRP", "A", "J", "LH", "LGT", "LS",
        "NLD", "T", "R", "TR",
    };
    QStringList parts = c.split('/', Qt::SkipEmptyParts);
    while (parts.size() > 1 && kTails.contains(parts.last()))
        parts.removeLast();
    if (parts.isEmpty()) return c;
    if (parts.size() == 1) return parts[0];
    // Shortest segment is the country designator (DL/N8EM, N8EM/DL) —
    // unless it is a bare area digit, which cty cannot resolve.
    QString shortest = parts[0], longest = parts[0];
    for (const QString& p : parts) {
        if (p.size() < shortest.size()) shortest = p;
        if (p.size() >= longest.size()) longest = p;
    }
    bool digitOnly = true;
    for (QChar ch : shortest)
        if (!ch.isDigit()) { digitOnly = false; break; }
    return digitOnly ? longest : shortest;
}

bool loggableCall(const QString& call) {
    const QString c = call.trimmed();
    if (c.size() < 3 || c.contains('?')) return false;   // "W1A?" is a fill request
    bool letter = false, digit = false;
    for (QChar ch : c) {
        if (ch.isLetter()) letter = true;
        else if (ch.isDigit()) digit = true;
    }
    return letter && digit;
}

bool nearMissCall(const QString& a, const QString& b) {
    if (a == b) return false;
    const int la = int(a.size()), lb = int(b.size());
    if (qAbs(la - lb) > 1) return false;
    if (la == lb) {                      // one substitution
        int diff = 0;
        for (int i = 0; i < la; ++i)
            if (a[i] != b[i] && ++diff > 1) return false;
        return diff == 1;
    }
    // One insertion: walk the longer string past a single skip.
    const QString& s = la < lb ? a : b;
    const QString& l = la < lb ? b : a;
    int i = 0, j = 0;
    bool skipped = false;
    while (i < s.size() && j < l.size()) {
        if (s[i] == l[j]) {
            ++i;
            ++j;
        } else if (!skipped) {
            skipped = true;
            ++j;
        } else {
            return false;
        }
    }
    return true;
}

int vkSlot(const QString& macroText) {
    const QString t = macroText.trimmed().toUpper();
    if (t.size() == 5 && t.startsWith("{VK") && t.endsWith('}')) {
        const int n = t.mid(3, 1).toInt();
        if (n >= 1 && n <= 4) return n;
    }
    return 0;
}

namespace {
// Prefix of a PLAIN call: everything through the call's LAST digit
// ("WA3ABC" -> WA3; "4X4AA" -> 4X4 — the digit after the X counts). A
// call with no digit at all (rare specials) takes first-two-plus-0.
QString plainPrefix(const QString& call) {
    int lastDigit = -1;
    for (int i = 0; i < call.size(); ++i)
        if (call[i].isDigit()) lastDigit = i;
    if (lastDigit < 0) return call.left(2) + '0';
    return call.left(lastDigit + 1);
}
} // namespace

QString wpxPrefix(const QString& call) {
    QString c = call.trimmed().toUpper();
    if (c.isEmpty()) return c;
    if (!c.contains('/')) return plainPrefix(c);
    static const QSet<QString> kTails = {
        "M", "MM", "P", "QRP", "A", "J", "LH", "LGT", "LS",
        "NLD", "T", "R", "TR",
    };
    QStringList parts = c.split('/', Qt::SkipEmptyParts);
    while (parts.size() > 1 && kTails.contains(parts.last()))
        parts.removeLast();
    if (parts.size() == 1) return plainPrefix(parts[0]);
    // Designator = the shortest segment (either side of the slash).
    QString desig = parts[0], home = parts[1];
    if (home.size() < desig.size()) std::swap(desig, home);
    bool digitOnly = true;
    for (QChar ch : desig)
        if (!ch.isDigit()) { digitOnly = false; break; }
    if (digitOnly) {
        // W1AW/4 -> the home prefix with its number swapped: W4.
        QString hp = plainPrefix(home);
        int i = hp.size() - 1;
        while (i >= 0 && hp[i].isDigit()) --i;
        return hp.left(i + 1) + desig;
    }
    bool hasDigit = false;
    for (QChar ch : desig)
        if (ch.isDigit()) { hasDigit = true; break; }
    return hasDigit ? plainPrefix(desig) : desig + '0';
}

QString formatSerial(int n, bool cut, int pad) {
    QString s = QString::number(n < 0 ? 0 : n);
    while (s.size() < pad) s.prepend('0');
    if (cut) {
        s.replace('0', 'T');
        s.replace('9', 'N');
    }
    return s;
}

ScoreBreakdown computeScore(const ContestDef& def,
                            const QList<CQsoValues>& qsos,
                            const CtyLookup* cty,
                            const ContestContext& ctx,
                            int qtcPoints) {
    ScoreBreakdown out;
    out.qtcPoints = qtcPoints;
    out.qsos = int(qsos.size());
    // Dupes log freely (working him again is legal and safe) but score
    // ZERO — six Enters on the same station must not claim six points.
    QSet<QString> seen;
    for (const CQsoValues& q : qsos) {
        QString dupeKey;
        switch (def.dupe) {
            case DupeScope::Never: break;
            case DupeScope::Contest: dupeKey = q.call; break;
            case DupeScope::PerBand: dupeKey = q.call + '|' + q.band; break;
            case DupeScope::PerBandMode:
                dupeKey = q.call + '|' + q.band + '|' + q.mode;
                break;
        }
        const bool dupe = !dupeKey.isEmpty() && seen.contains(dupeKey);
        if (!dupeKey.isEmpty()) seen.insert(dupeKey);
        CtyInfo ci;
        const bool ok =
            cty && cty->info(normalizeForCty(q.call), ci);
        const int pts =
            dupe ? 0 : (def.points ? def.points(q, ci, ok, ctx) : 0);
        out.points += pts;
        BandCount& bc = out.perBand[q.band];
        bc.qsos++;
        bc.points += pts;
        if (def.mults) {
            for (const QString& key : def.mults(q, ci, ok, ctx)) {
                if (key.isEmpty() || out.multKeys.contains(key)) continue;
                out.multKeys.insert(key);
                const int w = def.multWeight ? def.multWeight(key) : 1;
                out.mults++;
                out.weightedMults += w;
                bc.mults++;
                bc.weighted += w;
            }
        }
    }
    // No mults yet must still show the points earned, not zero.
    out.total = qint64(out.points + out.qtcPoints)
              * qint64(out.weightedMults > 0 ? out.weightedMults : 1);
    return out;
}

QList<qint64> allocateQtc(const QList<ContestQso>& qsos,
                          const QSet<qint64>& reportedIds,
                          const QString& toCall, int alreadySentTo) {
    const QString to = toCall.trimmed().toUpper();
    const int room = 10 - alreadySentTo;
    QList<qint64> out;
    if (room <= 0 || to.isEmpty()) return out;
    // qsos arrive oldest-first from the db; keep that order.
    for (const ContestQso& q : qsos) {
        if (out.size() >= qMin(10, room)) break;
        if (q.id < 0 || reportedIds.contains(q.id)) continue;
        if (q.v.call.compare(to, Qt::CaseInsensitive) == 0) continue;
        out << q.id;
    }
    return out;
}

int opTimeSecs(QList<QDateTime> events) { return opTimeSecs(std::move(events), 3600); }

int opTimeSecs(QList<QDateTime> events, int breakSecs) {
    if (events.size() < 2) return 0;
    std::sort(events.begin(), events.end());
    qint64 onAir = 0;
    for (int i = 1; i < events.size(); ++i) {
        const qint64 gap = events[i - 1].secsTo(events[i]);
        if (gap < breakSecs) onAir += gap;   // 59:59 of silence still counts
    }
    return int(onAir);
}

bool isDupe(const ContestDef& def, const QList<CQsoValues>& qsos,
            const QString& call, const QString& band, const QString& mode) {
    if (def.dupe == DupeScope::Never) return false;
    const QString c = call.trimmed().toUpper();
    for (const CQsoValues& q : qsos) {
        if (q.call.compare(c, Qt::CaseInsensitive) != 0) continue;
        switch (def.dupe) {
            case DupeScope::Contest: return true;
            case DupeScope::PerBand:
                if (q.band == band) return true;
                break;
            case DupeScope::PerBandMode:
                if (q.band == band && q.mode == mode) return true;
                break;
            case DupeScope::Never: return false;
        }
    }
    return false;
}

QString expandMacro(QString text, const ContestDef& def,
                    const ContestContext& ctx, const QString& hisCall,
                    const QString& sentExch, int sentSerial) {
    text.replace("{MYCALL}", ctx.myCall);
    text.replace("{HISCALL}", hisCall.trimmed().toUpper());
    text.replace("{SNT}", def.hasRst ? QStringLiteral("5NN") : QString());
    text.replace("{SENTNR}",
                 def.sentSerial
                     ? formatSerial(sentSerial, def.cutNumbers, def.serialPad)
                     : QString());
    text.replace("{EXCH}", sentExch);
    // Collapse doubled spaces a blank token leaves behind — "5NN  MI"
    // keys an audible extra word gap.
    return text.simplified();
}

QList<EsmAct> esmPlan(const EsmInput& in) {
    if (!in.esmOn) {
        // Plain Enter-logs: nothing ever transmits. An incomplete exchange
        // moves the cursor there (phone, 2026-10-23: call, Enter -> zone,
        // Enter -> log, the mic in between).
        if (in.callLoggable && in.exchComplete) return {EsmAct::Log};
        if (in.callLoggable) return {EsmAct::FocusExch};
        return {};
    }
    if (in.run) {
        if (in.callEmpty) return {EsmAct::KeyCq};
        // "W1A?" — key exactly that and nothing else: the standard
        // partial-call fill request (operator, 2026-10-22). The exchange
        // waits until the call is whole.
        if (in.callPartial) return {EsmAct::KeyHisCall};
        if (!in.callLoggable) {
            // Never offer the log for a call the save would refuse —
            // ask for the fill instead (his call + exchange).
            return {EsmAct::KeyHisCall, EsmAct::KeyExch};
        }
        if (!in.exchSent)
            // The answering Enter: even with the exchange already typed
            // (both orders happen on the air), answer BEFORE closing.
            return {EsmAct::KeyHisCall, EsmAct::KeyExch, EsmAct::FocusExch};
        if (in.exchComplete) return {EsmAct::KeyTu, EsmAct::Log};
        return {};                   // answered, waiting on his numbers
    }
    // S&P. No CQ from here, ever — and an empty call box keys nothing:
    // beat one fires at a station you have at least started to type.
    if (in.callEmpty) return {};
    if (!in.myCallSent) return {EsmAct::KeyMyCall, EsmAct::FocusExch};
    if (!in.exchSent) return {EsmAct::KeyExch};
    if (in.callLoggable && in.exchComplete) return {EsmAct::Log};
    return {};                       // beat three refused: incomplete QSO
}

// ---- Rate ----------------------------------------------------------------

RateStats rateStats(QList<QDateTime> ts, const QDateTime& now, int breakSecs) {
    RateStats r;
    if (ts.isEmpty()) return r;
    std::sort(ts.begin(), ts.end());
    for (const QDateTime& t : ts) {
        const qint64 a = t.secsTo(now);
        if (a <= 600) ++r.in10Min;
        if (a <= 3600) ++r.in60Min;
    }
    const auto pace = [&](int n) {
        if (ts.size() < n) return 0;
        const qint64 span = ts[ts.size() - n].secsTo(now);
        return span > 0 ? int(qint64(n) * 3600 / span) : 0;
    };
    r.last10QsoRate = pace(10);
    r.last100QsoRate = pace(100);
    QList<QDateTime> ev = ts;
    ev << now;                       // the stretch since the last QSO counts
    r.onSecs = opTimeSecs(ev, breakSecs);   // until it becomes a break
    r.offSecs = int(std::max<qint64>(0, ts.first().secsTo(now) - r.onSecs));
    return r;
}

QMap<QDateTime, int> qsosByHour(const QList<QDateTime>& ts) {
    QMap<QDateTime, int> m;
    for (const QDateTime& t : ts) {
        const QDateTime u = t.toUTC();
        m[QDateTime(u.date(), QTime(u.time().hour(), 0), QTimeZone::UTC)]++;
    }
    return m;
}

int bestSixtyMinutes(QList<QDateTime> ts, QDateTime* startOut) {
    std::sort(ts.begin(), ts.end());
    int best = 0, j = 0;
    for (int i = 0; i < ts.size(); ++i) {
        while (ts[j].secsTo(ts[i]) >= 3600) ++j;
        if (i - j + 1 > best) {
            best = i - j + 1;
            if (startOut) *startOut = ts[j];
        }
    }
    return best;
}

// ---- Multiplier grid -----------------------------------------------------

QList<MultGridRow> multGrid(const QSet<QString>& multKeys, bool allCqZones) {
    QMap<QString, MultGridRow> rows;     // "kind\x1fvalue" -> row
    const auto rowFor = [&](const QString& kind, const QString& value) -> MultGridRow& {
        MultGridRow& r = rows[kind + QChar(0x1f) + value];
        r.kind = kind;
        r.value = value;
        return r;
    };
    if (allCqZones)
        for (int z = 1; z <= 40; ++z) rowFor("Z", QString::number(z));
    for (const QString& key : multKeys) {
        QString body = key, band;
        const int bar = key.lastIndexOf('|');
        if (bar > 0) {
            body = key.left(bar);
            band = key.mid(bar + 1);
        }
        QString kind;
        const int colon = body.indexOf(':');
        if (colon > 0 && colon <= 3) {
            kind = body.left(colon);
            body = body.mid(colon + 1);
        }
        MultGridRow& r = rowFor(kind, body);
        if (band.isEmpty()) r.anyBand = true;
        else r.bands.insert(band);
    }
    QList<MultGridRow> out = rows.values();
    // Kinds together; zones numerically, everything else alphabetically.
    std::sort(out.begin(), out.end(), [](const MultGridRow& a, const MultGridRow& b) {
        if (a.kind != b.kind) return a.kind > b.kind;   // Z before C before ""
        bool okA = false, okB = false;
        const int na = a.value.toInt(&okA), nb = b.value.toInt(&okB);
        if (okA && okB) return na < nb;
        return a.value < b.value;
    });
    return out;
}

// ---- N+1 -----------------------------------------------------------------

QStringList nPlusOne(const QString& call, const QSet<QString>& pool, int max) {
    const QString c = call.trimmed().toUpper();
    QStringList out;
    if (c.size() < 3) return out;
    for (const QString& p : pool) {
        if (p == c || std::abs(int(p.size()) - int(c.size())) > 1) continue;
        if (nearMissCall(p, c)) out << p;
    }
    std::sort(out.begin(), out.end());
    if (out.size() > max) out = out.mid(0, max);
    return out;
}

// ---- Log checks ----------------------------------------------------------

QStringList logChecks(const ContestDef& def, const ContestRow& row,
                      const QList<ContestQso>& qsos) {
    QStringList w;
    int noFreq = 0, holes = 0, dupes = 0, future = 0;
    QList<CQsoValues> seen;
    const QDateTime now = QDateTime::currentDateTimeUtc().addSecs(300);
    QList<QDateTime> ts;
    for (const ContestQso& q : qsos) {
        ts << q.tsUtc;
        if (q.freqHz <= 0) ++noFreq;
        if (q.tsUtc > now) ++future;
        for (const ExchFieldDef& f : def.fields) {
            if (!f.required) continue;
            const QString v = f.col == ExchCol::RstR    ? q.v.rstR
                            : f.col == ExchCol::SerialR ? q.v.serialR
                            : f.col == ExchCol::Exch1   ? q.v.exch1
                            : f.col == ExchCol::Exch2   ? q.v.exch2
                                                        : q.v.exch3;
            if (v.trimmed().isEmpty()) { ++holes; break; }
        }
        if (isDupe(def, seen, q.v.call, q.v.band, q.v.mode)) ++dupes;
        seen << q.v;
    }
    if (holes) w << QString("%1 QSO(s) are missing a required exchange field.").arg(holes);
    if (noFreq) w << QString("%1 QSO(s) have no frequency.").arg(noFreq);
    if (future) w << QString("%1 QSO(s) are time-stamped in the future — check the PC clock.").arg(future);
    if (dupes) w << QString("%1 dupe(s) logged — fine to submit (they score zero), "
                            "listed so nothing surprises you.").arg(dupes);
    if (def.maxOpHours > 0 && row.catOp.startsWith(QLatin1String("SINGLE"))) {
        const int breakSecs = (def.minOffMinutes > 0 ? def.minOffMinutes : 60) * 60;
        const int on = opTimeSecs(ts, breakSecs);
        if (on > def.maxOpHours * 3600)
            w << QString("On-air time %1:%2 is over the single-op limit of %3 h "
                         "(off periods must be %4 min or longer to count).")
                     .arg(on / 3600).arg((on % 3600) / 60, 2, 10, QChar('0'))
                     .arg(def.maxOpHours).arg(def.minOffMinutes);
    }
    if (row.catOp.startsWith(QLatin1String("MULTI")) && row.catTx == QLatin1String("ONE")) {
        // Multi-single 10-minute rule: once on a band, stay 10 minutes.
        int quick = 0;
        QDateTime bandSince;
        QString band;
        for (const ContestQso& q : qsos) {
            if (q.runSp != QLatin1String("R")) continue;
            if (q.v.band != band) {
                if (!band.isEmpty() && bandSince.secsTo(q.tsUtc) < 600) ++quick;
                band = q.v.band;
                bandSince = q.tsUtc;
            }
        }
        if (quick) w << QString("%1 run-station band change(s) inside 10 minutes "
                                "(multi-single 10-minute rule).").arg(quick);
    }
    return w;
}

// ---- Summary -------------------------------------------------------------

QString summaryText(const ContestDef& def, const ContestRow& row,
                    const QList<ContestQso>& qsos, const ScoreBreakdown& sb) {
    QString t;
    t += QString("%1 — %2\n\n").arg(def.title, row.title);
    t += QString("%1  %2  %3  %4\n").arg("Band", -6).arg("QSOs", 6).arg("Points", 7).arg("Mults", 6);
    QStringList bands = sb.perBand.keys();
    std::sort(bands.begin(), bands.end(), [](const QString& a, const QString& b) {
        return a.left(a.size() - 1).toDouble() > b.left(b.size() - 1).toDouble();
    });
    for (const QString& b : bands) {
        const BandCount& c = sb.perBand.value(b);
        t += QString("%1  %2  %3  %4\n").arg(b, -6).arg(c.qsos, 6).arg(c.points, 7).arg(c.mults, 6);
    }
    t += QString("%1  %2  %3  %4\n\n").arg("Total", -6).arg(sb.qsos, 6).arg(sb.points, 7)
             .arg(sb.weightedMults, 6);
    t += QString("Claimed score: %1\n").arg(QLocale::c().toString(qlonglong(sb.total)));
    QList<QDateTime> ts;
    for (const ContestQso& q : qsos) ts << q.tsUtc;
    if (!ts.isEmpty()) {
        const int on = opTimeSecs(ts, (def.minOffMinutes > 0 ? def.minOffMinutes : 30) * 60);
        t += QString("On-air time: %1:%2\n").arg(on / 3600).arg((on % 3600) / 60, 2, 10, QChar('0'));
        if (on > 0) t += QString("Average rate: %1 QSOs/h\n").arg(qint64(ts.size()) * 3600 / on);
        QDateTime bs;
        const int best = bestSixtyMinutes(ts, &bs);
        t += QString("Best 60 minutes: %1 QSOs from %2Z\n\n").arg(best).arg(bs.toUTC().toString("ddd HHmm"));
        t += "QSOs by hour (UTC):\n";
        const QMap<QDateTime, int> h = qsosByHour(ts);
        for (auto it = h.cbegin(); it != h.cend(); ++it)
            t += QString("  %1  %2  %3\n").arg(it.key().toString("ddd HH")).arg(it.value(), 4)
                     .arg(QString(std::min(it.value(), 60), QChar('#')));
    }
    return t;
}

// ---- Online score --------------------------------------------------------

QString onlineScoreXml(const ContestDef& def, const ContestRow& row,
                       const ScoreBreakdown& sb, const OnlineScoreInfo& me,
                       const QDateTime& nowUtc) {
    const auto esc = [](QString v) {
        return v.replace('&', "&amp;").replace('<', "&lt;").replace('>', "&gt;")
                .replace('"', "&quot;");
    };
    QString x;
    x += "<?xml version=\"1.0\"?>\n<dynamicresults>\n";
    x += "<contest>" + esc(def.cabrilloName) + "</contest>\n";
    x += "<call>" + esc(me.call) + "</call>\n<ops>" + esc(me.call) + "</ops>\n";
    x += QString("<class power=\"%1\" assisted=\"%2\" transmitter=\"%3\" ops=\"%4\" "
                 "bands=\"%5\" mode=\"%6\" overlay=\"%7\"/>\n")
             .arg(esc(row.catPower), esc(row.catAssisted), esc(row.catTx), esc(row.catOp),
                  esc(row.catBand), esc(row.catMode), esc(row.catOverlay));
    x += "<club>" + esc(me.club) + "</club>\n";
    x += "<soft>tentec-console</soft>\n<version>" + esc(me.softVersion) + "</version>\n";
    x += QString("<qth><cqzone>%1</cqzone><iaruzone>%2</iaruzone>"
                 "<arrlsection>%3</arrlsection><stprvoth>%3</stprvoth>"
                 "<grid6>%4</grid6></qth>\n")
             .arg(me.cqZone).arg(me.ituZone).arg(esc(me.location), esc(me.grid));
    x += "<breakdown>\n";
    x += QString("<qso band=\"total\" mode=\"ALL\">%1</qso>\n").arg(sb.qsos);
    x += QString("<point band=\"total\" mode=\"ALL\">%1</point>\n").arg(sb.points + sb.qtcPoints);
    x += QString("<mult band=\"total\" mode=\"ALL\">%1</mult>\n").arg(sb.weightedMults);
    QStringList bands = sb.perBand.keys();
    std::sort(bands.begin(), bands.end());
    for (const QString& b : bands) {
        const BandCount& c = sb.perBand.value(b);
        const QString bn = b.endsWith('M') ? b.left(b.size() - 1) : b;   // "20M" -> "20"
        x += QString("<qso band=\"%1\" mode=\"ALL\">%2</qso>\n").arg(bn).arg(c.qsos);
        x += QString("<point band=\"%1\" mode=\"ALL\">%2</point>\n").arg(bn).arg(c.points);
        x += QString("<mult band=\"%1\" mode=\"ALL\">%2</mult>\n").arg(bn).arg(c.mults);
    }
    x += "</breakdown>\n";
    x += QString("<score>%1</score>\n").arg(sb.total);
    x += "<timestamp>" + nowUtc.toUTC().toString("yyyy-MM-dd HH:mm:ss") + "</timestamp>\n";
    x += "</dynamicresults>\n";
    return x;
}

} // namespace ttc
