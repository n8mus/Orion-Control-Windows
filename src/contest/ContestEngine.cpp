// SPDX-License-Identifier: GPL-2.0-or-later
#include "contest/ContestEngine.h"

#include <algorithm>

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
    if (c.size() < 3) return false;
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

int opTimeSecs(QList<QDateTime> events) {
    if (events.size() < 2) return 0;
    std::sort(events.begin(), events.end());
    qint64 onAir = 0;
    for (int i = 1; i < events.size(); ++i) {
        const qint64 gap = events[i - 1].secsTo(events[i]);
        if (gap < 3600) onAir += gap;   // 59:59 of silence still counts
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
        // Plain Enter-logs: the window handles the refusal messages.
        if (in.callLoggable && in.exchComplete) return {EsmAct::Log};
        return {};
    }
    if (in.run) {
        if (in.callEmpty) return {EsmAct::KeyCq};
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

} // namespace ttc
